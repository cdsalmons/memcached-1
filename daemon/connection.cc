/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"
#include "memcached.h"
#include "runtime.h"
#include <exception>
#include <utilities/protocol2text.h>
#include <platform/strerror.h>

Connection::Connection()
    : socketDescriptor(INVALID_SOCKET),
      max_reqs_per_event(settings.default_reqs_per_event),
      numEvents(0),
      sasl_conn(nullptr),
      stateMachine(new McbpStateMachine(conn_immediate_close)),
      protocol(Protocol::Memcached),
      admin(false),
      authenticated(false),
      username("unknown"),
      registered_in_libevent(false),
      ev_flags(0),
      currentEvent(0),
      write_and_go(nullptr),
      ritem(nullptr),
      rlbytes(0),
      item(nullptr),
      iov(IOV_LIST_INITIAL),
      iovused(0),
      msglist(),
      msgcurr(0),
      msgbytes(0),
      noreply(false),
      nodelay(false),
      refcount(0),
      supports_datatype(false),
      supports_mutation_extras(false),
      engine_storage(nullptr),
      start(0),
      cas(0),
      cmd(PROTOCOL_BINARY_CMD_INVALID),
      next(nullptr),
      thread(nullptr),
      aiostat(ENGINE_SUCCESS),
      ewouldblock(false),
      parent_port(0),
      tap_iterator(nullptr),
      dcp(false),
      commandContext(nullptr),
      auth_context(nullptr),
      bucketIndex(0),
      bucketEngine(nullptr),
      peername("unknown"),
      sockname("unknown") {
    MEMCACHED_CONN_CREATE(this);

    memset(&event, 0, sizeof(event));
    memset(&read, 0, sizeof(read));
    memset(&write, 0, sizeof(write));
    memset(&ssl, 0, sizeof(ssl));
    memset(&binary_header, 0, sizeof(binary_header));

    msglist.reserve(MSG_LIST_INITIAL);
    setState(conn_immediate_close);
    socketDescriptor = INVALID_SOCKET;
}

Connection::Connection(SOCKET sock, const struct listening_port &interface)
    : socketDescriptor(sock),
      max_reqs_per_event(settings.default_reqs_per_event),
      numEvents(0),
      sasl_conn(nullptr),
      stateMachine(interface.protocol == Protocol::Memcached ? new McbpStateMachine(conn_new_cmd) : nullptr),
      protocol(interface.protocol),
      admin(false),
      authenticated(false),
      username("unknown"),
      registered_in_libevent(false),
      ev_flags(0),
      currentEvent(0),
      write_and_go(conn_new_cmd),
      ritem(nullptr),
      rlbytes(0),
      item(nullptr),
      iov(IOV_LIST_INITIAL),
      iovused(0),
      msglist(),
      msgcurr(0),
      msgbytes(0),
      noreply(false),
      nodelay(false),
      refcount(0),
      supports_datatype(false),
      supports_mutation_extras(false),
      engine_storage(nullptr),
      start(0),
      cas(0),
      cmd(PROTOCOL_BINARY_CMD_INVALID),
      next(nullptr),
      thread(nullptr),
      aiostat(ENGINE_SUCCESS),
      ewouldblock(false),
      parent_port(interface.port),
      tap_iterator(nullptr),
      dcp(false),
      commandContext(nullptr),
      auth_context(nullptr),
      bucketIndex(0),
      bucketEngine(nullptr),
      peername("unknown"),
      sockname("unknown") {
    MEMCACHED_CONN_CREATE(this);

    memset(&event, 0, sizeof(event));
    memset(&read, 0, sizeof(read));
    memset(&write, 0, sizeof(write));
    memset(&ssl, 0, sizeof(ssl));
    memset(&binary_header, 0, sizeof(binary_header));

    msglist.reserve(MSG_LIST_INITIAL);

    resolveConnectionName(false);
    setAuthContext(auth_create(NULL, peername.c_str(),
                               sockname.c_str()));

    setTcpNoDelay(interface.tcp_nodelay);
    if (interface.ssl.enabled) {
        if (!enableSSL(interface.ssl.cert, interface.ssl.key)) {
            throw std::runtime_error(std::to_string(getId()) +
                                     " Failed to enable SSL");
        }
    }
}

Connection::~Connection() {
    MEMCACHED_CONN_DESTROY(this);
    auth_destroy(auth_context);
    cbsasl_dispose(&sasl_conn);
    free(read.buf);
    free(write.buf);

    releaseReservedItems();
    for (auto* ptr : temp_alloc) {
        free(ptr);
    }
}

void Connection::setState(TaskFunction next_state) {
    stateMachine->setCurrentTask(*this, next_state);
}

void Connection::runStateMachinery() {
    do {
        if (settings.verbose) {
            LOG_DEBUG(this, "%u - Running task: (%s)", getId(),
                      stateMachine->getCurrentTaskName());
        }
    } while (stateMachine->execute(*this));
}

/**
 * Convert a sockaddr_storage to a textual string (no name lookup).
 *
 * @param addr the sockaddr_storage received from getsockname or
 *             getpeername
 * @param addr_len the current length used by the sockaddr_storage
 * @return a textual string representing the connection. or NULL
 *         if an error occurs (caller takes ownership of the buffer and
 *         must call free)
 */
static std::string sockaddr_to_string(const struct sockaddr_storage* addr,
                                      socklen_t addr_len) {
    char host[50];
    char port[50];

    int err = getnameinfo(reinterpret_cast<const struct sockaddr*>(addr), addr_len,
                          host, sizeof(host),
                          port, sizeof(port),
                          NI_NUMERICHOST | NI_NUMERICSERV);
    if (err != 0) {
        LOG_WARNING(NULL, "getnameinfo failed with error %d", err);
        return NULL;
    }

    return std::string(host) + ":" + std::string(port);
}

void Connection::resolveConnectionName(bool listening) {
    int err;
    try {
        if (listening) {
            peername = "*";
        } else {
            struct sockaddr_storage peer;
            socklen_t peer_len = sizeof(peer);
            if ((err = getpeername(socketDescriptor, reinterpret_cast<struct sockaddr*>(&peer),
                                   &peer_len)) != 0) {
                LOG_WARNING(NULL, "getpeername for socket %d with error %d",
                            socketDescriptor, err);
            } else {
                peername = sockaddr_to_string(&peer, peer_len);
            }
        }

        struct sockaddr_storage sock;
        socklen_t sock_len = sizeof(sock);
        if ((err = getsockname(socketDescriptor, reinterpret_cast<struct sockaddr*>(&sock),
                               &sock_len)) != 0) {
            LOG_WARNING(NULL, "getsockname for socket %d with error %d",
                        socketDescriptor, err);
        } else {
            sockname = sockaddr_to_string(&sock, sock_len);
        }
    } catch (std::bad_alloc& e) {
        LOG_WARNING(NULL,
                    "Connection::resolveConnectionName: failed to allocate memory: %s",
                    e.what());
    }
}

bool Connection::unregisterEvent() {
    if (!registered_in_libevent) {
        LOG_WARNING(NULL,
                    "Connection::unregisterEvent: Not registered in libevent - "
                        "ignoring unregister attempt");
        return false;
    }

    cb_assert(socketDescriptor != INVALID_SOCKET);

    if (event_del(&event) == -1) {
        log_system_error(EXTENSION_LOG_WARNING,
                         NULL,
                         "Failed to remove connection to libevent: %s");
        return false;
    }

    registered_in_libevent = false;
    return true;
}

bool Connection::registerEvent() {
    if (registered_in_libevent) {
        LOG_WARNING(NULL, "Connection::registerEvent: Already registered in"
            " libevent - ignoring register attempt");
        return false;
    }

    cb_assert(socketDescriptor != INVALID_SOCKET);

    if (event_add(&event, NULL) == -1) {
        log_system_error(EXTENSION_LOG_WARNING,
                         NULL,
                         "Failed to add connection to libevent: %s");
        return false;
    }

    registered_in_libevent = true;

    return true;
}

bool Connection::updateEvent(const short new_flags) {
    struct event_base* base = event.ev_base;

    if (ssl.isEnabled() && ssl.isConnected() && (new_flags & EV_READ)) {
        /*
         * If we want more data and we have SSL, that data might be inside
         * SSL's internal buffers rather than inside the socket buffer. In
         * that case signal an EV_READ event without actually polling the
         * socket.
         */
        char dummy;
        /* SSL_pending() will not work here despite the name */
        int rv = ssl.peek(&dummy, 1);
        if (rv > 0) {
            /* signal a call to the handler */
            event_active(&event, EV_READ, 0);
            return true;
        }
    }

    if (ev_flags == new_flags) {
        // There is no change in the event flags!
        return true;
    }

    if (settings.verbose > 1) {
        LOG_DEBUG(NULL, "Updated event for %u to read=%s, write=%s\n",
                  getId(), (new_flags & EV_READ ? "yes" : "no"),
                  (new_flags & EV_WRITE ? "yes" : "no"));
    }

    if (!unregisterEvent()) {
        LOG_DEBUG(this,
                  "Failed to remove connection from event notification "
                      "library. Shutting down connection [%s - %s]",
                  getPeername().c_str(),
                  getSockname().c_str());
        return false;
    }

    event_set(&event, socketDescriptor, new_flags, event_handler,
              reinterpret_cast<void*>(this));
    event_base_set(base, &event);
    ev_flags = new_flags;

    if (!registerEvent()) {
        LOG_DEBUG(this,
                  "Failed to add connection to the event notification "
                      "library. Shutting down connection [%s - %s]",
                  getPeername().c_str(),
                  getSockname().c_str());
        return false;
    }

    return true;
}

bool Connection::initializeEvent(struct event_base* base) {
    short event_flags = (EV_READ | EV_PERSIST);
    event_set(&event, socketDescriptor, event_flags, event_handler,
              reinterpret_cast<void*>(this));
    event_base_set(base, &event);
    ev_flags = event_flags;

    return registerEvent();
}

bool Connection::setTcpNoDelay(bool enable) {
    int flags = enable ? 1 : 0;

#if defined(WIN32)
    char* flags_ptr = reinterpret_cast<char*>(&flags);
#else
    void* flags_ptr = reinterpret_cast<void*>(&flags);
#endif
    int error = setsockopt(socketDescriptor, IPPROTO_TCP, TCP_NODELAY, flags_ptr,
                           sizeof(flags));

    if (error != 0) {
        std::string errmsg = cb_strerror(GetLastNetworkError());
        LOG_WARNING(this, "setsockopt(TCP_NODELAY): %s",
                    errmsg.c_str());
        nodelay = false;
        return false;
    } else {
        nodelay = enable;
    }

    return true;
}

/* cJSON uses double for all numbers, so only has 53 bits of precision.
 * Therefore encode 64bit integers as string.
 */
static cJSON* json_create_uintptr(uintptr_t value) {
    char buffer[32];
    if (snprintf(buffer, sizeof(buffer),
                 "0x%" PRIxPTR, value) >= int(sizeof(buffer))) {
        return cJSON_CreateString("<too long>");
    } else {
        return cJSON_CreateString(buffer);
    }
}

static void json_add_uintptr_to_object(cJSON* obj, const char* name,
                                       uintptr_t value) {
    cJSON_AddItemToObject(obj, name, json_create_uintptr(value));
}

static void json_add_bool_to_object(cJSON* obj, const char* name, bool value) {
    if (value) {
        cJSON_AddTrueToObject(obj, name);
    } else {
        cJSON_AddFalseToObject(obj, name);
    }
}

const char* to_string(const Protocol& protocol) {
    if (protocol == Protocol::Memcached) {
        return "memcached";
    } else if (protocol == Protocol::Greenstack) {
        return "greenstack";
    } else {
        return "unknown";
    }
}

cJSON* Connection::toJSON() const {
    cJSON* obj = cJSON_CreateObject();
    json_add_uintptr_to_object(obj, "connection", (uintptr_t) this);
    if (socketDescriptor == INVALID_SOCKET) {
        cJSON_AddStringToObject(obj, "socket", "disconnected");
    } else {
        cJSON_AddNumberToObject(obj, "socket", (double) socketDescriptor);
        cJSON_AddStringToObject(obj, "protocol", to_string(getProtocol()));
        cJSON_AddStringToObject(obj, "peername", getPeername().c_str());
        cJSON_AddStringToObject(obj, "sockname", getSockname().c_str());
        cJSON_AddNumberToObject(obj, "max_reqs_per_event",
                                max_reqs_per_event);
        cJSON_AddNumberToObject(obj, "nevents", numEvents);
        json_add_bool_to_object(obj, "admin", isAdmin());
        if (sasl_conn != NULL) {
            json_add_uintptr_to_object(obj, "sasl_conn",
                                       (uintptr_t) sasl_conn);
        }
        {
            cJSON* state = cJSON_CreateArray();
            cJSON_AddItemToArray(state,
                                 cJSON_CreateString(stateMachine->getCurrentTaskName()));
            cJSON_AddItemToObject(obj, "state", state);
        }
        json_add_bool_to_object(obj, "registered_in_libevent",
                                isRegisteredInLibevent());
        json_add_uintptr_to_object(obj, "ev_flags",
                                   (uintptr_t) getEventFlags());
        json_add_uintptr_to_object(obj, "which", (uintptr_t) getCurrentEvent());
        {
            cJSON* readobj = cJSON_CreateObject();
            json_add_uintptr_to_object(readobj, "buf", (uintptr_t) read.buf);
            json_add_uintptr_to_object(readobj, "curr", (uintptr_t) read.curr);
            cJSON_AddNumberToObject(readobj, "size", read.size);
            cJSON_AddNumberToObject(readobj, "bytes", read.bytes);

            cJSON_AddItemToObject(obj, "read", readobj);
        }
        {
            cJSON* writeobj = cJSON_CreateObject();
            json_add_uintptr_to_object(writeobj, "buf", (uintptr_t) write.buf);
            json_add_uintptr_to_object(writeobj, "curr",
                                       (uintptr_t) write.curr);
            cJSON_AddNumberToObject(writeobj, "size", write.size);
            cJSON_AddNumberToObject(writeobj, "bytes", write.bytes);

            cJSON_AddItemToObject(obj, "write", writeobj);
        }
        json_add_uintptr_to_object(obj, "write_and_go",
                                   (uintptr_t) write_and_go);
        json_add_uintptr_to_object(obj, "write_and_free",
                                   (uintptr_t) write_and_free);
        json_add_uintptr_to_object(obj, "ritem", (uintptr_t) ritem);
        cJSON_AddNumberToObject(obj, "rlbytes", rlbytes);
        json_add_uintptr_to_object(obj, "item", (uintptr_t) item);
        {
            cJSON* iovobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(iovobj, "size", iov.size());
            cJSON_AddNumberToObject(iovobj, "used", iovused);

            cJSON_AddItemToObject(obj, "iov", iovobj);
        }
        {
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddNumberToObject(msg, "size", msglist.capacity());
            cJSON_AddNumberToObject(msg, "used", msglist.size());
            cJSON_AddNumberToObject(msg, "curr", msgcurr);
            cJSON_AddNumberToObject(msg, "bytes", msgbytes);

            cJSON_AddItemToObject(obj, "msglist", msg);
        }
        {
            cJSON* ilist = cJSON_CreateObject();
            cJSON_AddNumberToObject(ilist, "size", reservedItems.size());
            cJSON_AddItemToObject(obj, "itemlist", ilist);
        }
        {
            cJSON* talloc = cJSON_CreateObject();
            cJSON_AddNumberToObject(talloc, "size", temp_alloc.size());
            cJSON_AddItemToObject(obj, "temp_alloc_list", talloc);
        }
        json_add_bool_to_object(obj, "noreply", noreply);
        json_add_bool_to_object(obj, "nodelay", nodelay);
        cJSON_AddNumberToObject(obj, "refcount", refcount);
        {
            cJSON* features = cJSON_CreateObject();
            json_add_bool_to_object(features, "datatype",
                                    supports_datatype);
            json_add_bool_to_object(features, "mutation_extras",
                                    supports_mutation_extras);

            cJSON_AddItemToObject(obj, "features", features);
        }
        {
            cJSON* dy_buf = cJSON_CreateObject();
            json_add_uintptr_to_object(dy_buf, "buffer",
                                       (uintptr_t) dynamicBuffer.getRoot());
            cJSON_AddNumberToObject(dy_buf, "size",
                                    (double) dynamicBuffer.getSize());
            cJSON_AddNumberToObject(dy_buf, "offset",
                                    (double) dynamicBuffer.getOffset());

            cJSON_AddItemToObject(obj, "DynamicBuffer", dy_buf);
        }
        json_add_uintptr_to_object(obj, "engine_storage",
                                   (uintptr_t) engine_storage);
        /* @todo we should decode the binary header */
        json_add_uintptr_to_object(obj, "cas", cas);
        {
            cJSON* cmdarr = cJSON_CreateArray();
            cJSON_AddItemToArray(cmdarr, json_create_uintptr(cmd));
            const char* cmd_name = memcached_opcode_2_text(cmd);
            if (cmd_name == NULL) {
                cmd_name = "";
            }
            cJSON_AddItemToArray(cmdarr, cJSON_CreateString(cmd_name));

            cJSON_AddItemToObject(obj, "cmd", cmdarr);
        }
        json_add_uintptr_to_object(obj, "opaque", getOpaque());
        json_add_uintptr_to_object(obj, "next", (uintptr_t) next);
        json_add_uintptr_to_object(obj, "thread", (uintptr_t)thread.load(
            std::memory_order::memory_order_relaxed));
        cJSON_AddNumberToObject(obj, "aiostat", aiostat);
        json_add_bool_to_object(obj, "ewouldblock", ewouldblock);
        json_add_uintptr_to_object(obj, "tap_iterator",
                                   (uintptr_t) tap_iterator);
        cJSON_AddNumberToObject(obj, "parent_port", parent_port);
        json_add_bool_to_object(obj, "dcp", dcp);

        {
            cJSON* sslobj = cJSON_CreateObject();
            json_add_bool_to_object(sslobj, "enabled", ssl.isEnabled());
            if (ssl.isEnabled()) {
                json_add_bool_to_object(sslobj, "connected", ssl.isConnected());
            }

            cJSON_AddItemToObject(obj, "ssl", sslobj);
        }
    }
    return obj;
}

void Connection::shrinkBuffers() {
    if (read.size > READ_BUFFER_HIGHWAT && read.bytes < DATA_BUFFER_SIZE) {
        if (read.curr != read.buf) {
            /* Pack the buffer */
            memmove(read.buf, read.curr, (size_t)read.bytes);
        }

        void* ptr = realloc(read.buf, DATA_BUFFER_SIZE);
        char* newbuf = reinterpret_cast<char*>(ptr);
        if (newbuf) {
            read.buf = newbuf;
            read.size = DATA_BUFFER_SIZE;
        } else {
            LOG_WARNING(this,
                        "%u: Failed to shrink read buffer down to %" PRIu64
                            " bytes.", getId(), DATA_BUFFER_SIZE);
        }
        read.curr = read.buf;
    }

    if (msglist.size() > MSG_LIST_HIGHWAT) {
        try {
            msglist.resize(MSG_LIST_INITIAL);
            msglist.shrink_to_fit();
        } catch (std::bad_alloc) {
            LOG_WARNING(this,
                        "%u: Failed to shrink msglist down to %" PRIu64
                            " elements.", getId(),
                        MSG_LIST_INITIAL);
        }
    }

    if (iov.size() > IOV_LIST_HIGHWAT) {
        try {
            iov.resize(IOV_LIST_INITIAL);
            iov.shrink_to_fit();
        } catch (std::bad_alloc) {
            LOG_WARNING(this,
                        "%u: Failed to shrink iov down to %" PRIu64
                            " elements.", getId(),
                        IOV_LIST_INITIAL);
        }
    }

    // The DynamicBuffer is only occasionally used - free the whole thing
    // if it's still allocated.
    dynamicBuffer.clear();
}


int Connection::sslPreConnection() {
    int r = ssl.accept();
    if (r == 1) {
        ssl.drainBioSendPipe(socketDescriptor);
        ssl.setConnected();
    } else {
        if (ssl.getError(r) == SSL_ERROR_WANT_READ) {
            ssl.drainBioSendPipe(socketDescriptor);
            set_ewouldblock();
            return -1;
        } else {
            try {
                std::string errmsg("SSL_accept() returned " +
                                   std::to_string(r) +
                                   " with error " +
                                   std::to_string(ssl.getError(r)));

                std::vector<char> ssl_err(1024);
                ERR_error_string_n(ERR_get_error(), ssl_err.data(),
                                   ssl_err.size());

                LOG_WARNING(this, "%u: ERROR: %s\n%s",
                            getId(), errmsg.c_str(), ssl_err.data());
            } catch (const std::bad_alloc&) {
                // unable to print error message; continue.
            }

            set_econnreset();
            return -1;
        }
    }

    return 0;
}

int Connection::recv(char* dest, size_t nbytes) {
    int res;
    if (ssl.isEnabled()) {
        ssl.drainBioRecvPipe(socketDescriptor);

        if (ssl.hasError()) {
            set_econnreset();
            return -1;
        }

        if (!ssl.isConnected()) {
            res = sslPreConnection();
            if (res == -1) {
                return -1;
            }
        }

        /* The SSL negotiation might be complete at this time */
        if (ssl.isConnected()) {
            res = sslRead(dest, nbytes);
        }
    } else {
        if (isPipeConnection()) {
            res = (int)::read(socketDescriptor, dest, nbytes);
        } else {
            res = (int)::recv(socketDescriptor, dest, nbytes, 0);
        }
    }

    return res;
}

int Connection::sendmsg(struct msghdr* m) {
    int res = 0;
    if (ssl.isEnabled()) {
        for (int ii = 0; ii < int(m->msg_iovlen); ++ii) {
            int n = sslWrite(reinterpret_cast<char*>(m->msg_iov[ii].iov_base),
                             m->msg_iov[ii].iov_len);
            if (n > 0) {
                res += n;
            } else {
                return res > 0 ? res : -1;
            }
        }

        /* @todo figure out how to drain the rest of the data if we
         * failed to send all of it...
         */
        ssl.drainBioSendPipe(socketDescriptor);
        return res;
    } else {
        if (isPipeConnection()) {
            // Windows and POSIX safe, manually write the scatter/gather
            for (size_t ii = 0; ii < size_t(m->msg_iovlen); ii++) {
                res = ::write(fileno(stdout),
                              m->msg_iov[ii].iov_base,
                              m->msg_iov[ii].iov_len);
            }
        }
        else {
            res = ::sendmsg(socketDescriptor, m, 0);
        }
    }

    return res;
}

Connection::TransmitResult Connection::transmit() {
    while (msgcurr < msglist.size() &&
           msglist[msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        msgcurr++;
    }

    if (msgcurr < msglist.size()) {
        ssize_t res;
        struct msghdr *m = &msglist[msgcurr];

        res = sendmsg(m);
        auto error = GetLastNetworkError();
        if (res > 0) {
            get_thread_stats(this)->bytes_written += res;

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= ssize_t(m->msg_iov->iov_len)) {
                res -= (ssize_t)m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base = (void*)((unsigned char*)m->msg_iov->iov_base + res);
                m->msg_iov->iov_len -= res;
            }
            return TransmitResult::Incomplete;
        }

        if (res == -1 && is_blocking(error)) {
            if (!updateEvent(EV_WRITE | EV_PERSIST)) {
                setState(conn_closing);
                return TransmitResult::HardError;
            }
            return TransmitResult::SoftError;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0) {
            if (res == -1) {
                log_socket_error(EXTENSION_LOG_WARNING, this,
                                 "Failed to write, and not due to blocking: %s");
            } else {
                LOG_WARNING(this, "%d - sendmsg returned 0\n",
                            socketDescriptor);
                for (int ii = 0; ii < int(m->msg_iovlen); ++ii) {
                    LOG_WARNING(this, "\t%d - %zu\n",
                                socketDescriptor, m->msg_iov[ii].iov_len);
                }
            }
        }

        setState(conn_closing);
        return TransmitResult::HardError;
    } else {
        if (ssl.isEnabled()) {
            ssl.drainBioSendPipe(socketDescriptor);
            if (ssl.morePendingOutput()) {
                if (!updateEvent(EV_WRITE | EV_PERSIST)) {
                    setState(conn_closing);
                    return TransmitResult::HardError;
                }
                return TransmitResult::SoftError;
            }
        }

        return TransmitResult::Complete;
    }
}

/**
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start
 * looking at the data I've got after a number of reallocs...
 */
Connection::TryReadResult Connection::tryReadNetwork() {
    TryReadResult gotdata = TryReadResult::NoDataReceived;
    int res;
    int num_allocs = 0;

    if (read.curr != read.buf) {
        if (read.bytes != 0) { /* otherwise there's nothing to copy */
            memmove(read.buf, read.curr, read.bytes);
        }
        read.curr = read.buf;
    }

    while (1) {
        int avail;
        if (read.bytes >= read.size) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char* new_rbuf = reinterpret_cast<char*>(realloc(read.buf,
                                                             read.size * 2));
            if (!new_rbuf) {
                LOG_WARNING(this, "Couldn't realloc input buffer");
                read.bytes = 0; /* ignore what we read */
                setState(conn_closing);
                return TryReadResult::MemoryError;
            }
            read.curr = read.buf = new_rbuf;
            read.size *= 2;
        }

        avail = read.size - read.bytes;
        res = recv(read.buf + read.bytes, avail);
        if (res > 0) {
            get_thread_stats(this)->bytes_read += res;
            gotdata = TryReadResult::DataReceived;
            read.bytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return isPipeConnection() ?
                   TryReadResult::NoDataReceived : TryReadResult::SocketError;
        }
        if (res == -1) {
            auto error = GetLastNetworkError();

            if (is_blocking(error)) {
                break;
            }
            char prefix[160];
            snprintf(prefix, sizeof(prefix),
                     "%u Closing connection [%s - %s] due to read error: %%s",
                     getId(), getPeername().c_str(), getSockname().c_str());
            log_errcode_error(EXTENSION_LOG_WARNING, this, prefix, error);

            return TryReadResult::SocketError;
        }
    }
    return gotdata;
}

int Connection::sslRead(char* dest, size_t nbytes) {
    int ret = 0;

    while (ret < int(nbytes)) {
        int n;
        ssl.drainBioRecvPipe(socketDescriptor);
        if (ssl.hasError()) {
            set_econnreset();
            return -1;
        }
        n = ssl.read(dest + ret, (int)(nbytes - ret));
        if (n > 0) {
            ret += n;
        } else {
            /* n < 0 and n == 0 require a check of SSL error*/
            int error = ssl.getError(n);

            switch (error) {
            case SSL_ERROR_WANT_READ:
                /*
                 * Drain the buffers and retry if we've got data in
                 * our input buffers
                 */
                if (ssl.moreInputAvailable()) {
                    /* our recv buf has data feed the BIO */
                    ssl.drainBioRecvPipe(socketDescriptor);
                } else if (ret > 0) {
                    /* nothing in our recv buf, return what we have */
                    return ret;
                } else {
                    set_ewouldblock();
                    return -1;
                }
                break;

            case SSL_ERROR_ZERO_RETURN:
                /* The TLS/SSL connection has been closed (cleanly). */
                return 0;

            default:
                /*
                 * @todo I don't know how to gracefully recover from this
                 * let's just shut down the connection
                 */
                LOG_WARNING(this,
                            "%u: ERROR: SSL_read returned -1 with error %d",
                            getId(), error);
                set_econnreset();
                return -1;
            }
        }
    }

    return ret;
}

int Connection::sslWrite(const char* src, size_t nbytes) {
    int ret = 0;

    int chunksize = settings.bio_drain_buffer_sz;

    while (ret < int(nbytes)) {
        int n;
        int chunk;

        ssl.drainBioSendPipe(socketDescriptor);
        if (ssl.hasError()) {
            set_econnreset();
            return -1;
        }

        chunk = (int)(nbytes - ret);
        if (chunk > chunksize) {
            chunk = chunksize;
        }

        n = ssl.write(src + ret, chunk);
        if (n > 0) {
            ret += n;
        } else {
            if (ret > 0) {
                /* We've sent some data.. let the caller have them */
                return ret;
            }

            if (n < 0) {
                int error = ssl.getError(n);
                switch (error) {
                case SSL_ERROR_WANT_WRITE:
                    set_ewouldblock();
                    return -1;

                default:
                    /*
                     * @todo I don't know how to gracefully recover from this
                     * let's just shut down the connection
                     */
                    LOG_WARNING(this,
                                "%u: ERROR: SSL_write returned -1 with error %d",
                                getId(), error);
                    set_econnreset();
                    return -1;
                }
            }
        }
    }

    return ret;
}

SslContext::~SslContext() {
    if (enabled) {
        disable();
    }
}

bool SslContext::enable(const std::string& cert, const std::string& pkey) {
    ctx = SSL_CTX_new(SSLv23_server_method());

    /* MB-12359 - Disable SSLv2 & SSLv3 due to POODLE */
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    /* @todo don't read files, but use in-memory-copies */
    if (!SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) ||
        !SSL_CTX_use_PrivateKey_file(ctx, pkey.c_str(), SSL_FILETYPE_PEM)) {
        return false;
    }

    set_ssl_ctx_cipher_list(ctx);

    enabled = true;
    error = false;
    client = NULL;

    try {
        in.buffer.resize(settings.bio_drain_buffer_sz);
        out.buffer.resize(settings.bio_drain_buffer_sz);
    } catch (std::bad_alloc) {
        return false;
    }

    BIO_new_bio_pair(&application, in.buffer.size(),
                     &network, out.buffer.size());

    client = SSL_new(ctx);
    SSL_set_bio(client, application, application);

    return true;
}

void SslContext::disable() {
    if (network != nullptr) {
        BIO_free_all(network);
    }
    if (client != nullptr) {
        SSL_free(client);
    }
    error = false;
    if (ctx != nullptr) {
        SSL_CTX_free(ctx);
    }
    enabled = false;
}

void SslContext::drainBioRecvPipe(SOCKET sfd) {
    int n;
    bool stop = false;

    do {
        if (in.current < in.total) {
            n = BIO_write(network, in.buffer.data() + in.current,
                          int(in.total - in.current));
            if (n > 0) {
                in.current += n;
                if (in.current == in.total) {
                    in.current = in.total = 0;
                }
            } else {
                /* Our input BIO is full, no need to grab more data from
                 * the network at this time..
                 */
                return;
            }
        }

        if (in.total < in.buffer.size()) {
            n = recv(sfd, in.buffer.data() + in.total,
                     in.buffer.size() - in.total, 0);
            if (n > 0) {
                in.total += n;
            } else {
                stop = true;
                if (n == 0) {
                    error = true; /* read end shutdown */
                } else {
                    if (!is_blocking(GetLastNetworkError())) {
                        error = true;
                    }
                }
            }
        }
    } while (!stop);
}

void SslContext::drainBioSendPipe(SOCKET sfd) {
    int n;
    bool stop = false;

    do {
        if (out.current < out.total) {
            n = send(sfd, out.buffer.data() + out.current,
                     out.total - out.current, 0);
            if (n > 0) {
                out.current += n;
                if (out.current == out.total) {
                    out.current = out.total = 0;
                }
            } else {
                if (n == -1) {
                    if (!is_blocking(GetLastNetworkError())) {
                        error = true;
                    }
                }
                return;
            }
        }

        if (out.total == 0) {
            n = BIO_read(network, out.buffer.data(), int(out.buffer.size()));
            if (n > 0) {
                out.total = n;
            } else {
                stop = true;
            }
        }
    } while (!stop);
}

void SslContext::dumpCipherList(uint32_t id) const {
    LOG_DEBUG(NULL, "%u: Using SSL ciphers:", id);
    int ii = 0;
    const char* cipher;
    while ((cipher = SSL_get_cipher_list(client, ii++)) != NULL) {
        LOG_DEBUG(NULL, "%u    %s", id, cipher);
    }
}

bool Connection::includeErrorStringInResponseBody(
                protocol_binary_response_status err) const {
    // Maintain backwards compatibility - return true for older commands which
    // have for some time returned included the error string. For newer
    // commands where there is no backwards compat issue, return false.

    // Note: skipping the error string is currently "opt-in", as I'm not
    // sure which commands other than these very new ones we can safely skip
    // the string and not cause client incompatibilities.
    switch (binary_header.request.opcode) {
    case PROTOCOL_BINARY_CMD_SUBDOC_GET:
    case PROTOCOL_BINARY_CMD_SUBDOC_EXISTS:
    case PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD:
    case PROTOCOL_BINARY_CMD_SUBDOC_DICT_UPSERT:
    case PROTOCOL_BINARY_CMD_SUBDOC_DELETE:
    case PROTOCOL_BINARY_CMD_SUBDOC_REPLACE:
    case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST:
    case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST:
    case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT:
    case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE:
    case PROTOCOL_BINARY_CMD_SUBDOC_COUNTER:
    case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP:
    case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION:
        return false;

    default:
        // Some legacy commands don't return the error string for specifie
        // error codes:
        switch (err) {
        case PROTOCOL_BINARY_RESPONSE_SUCCESS:
        case PROTOCOL_BINARY_RESPONSE_NOT_INITIALIZED:
        case PROTOCOL_BINARY_RESPONSE_AUTH_STALE:
        case PROTOCOL_BINARY_RESPONSE_NO_BUCKET:
            return false;

        default:
            return true;
        }
    }
}

bool Connection::addMsgHdr(bool reset) {
    if (reset) {
        msgcurr = 0;
        msglist.clear();
        iovused = 0;
    }

    try {
        msglist.emplace_back();
    } catch (std::bad_alloc&) {
        return false;
    }

    struct msghdr& msg = msglist.back();

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(&msg, 0, sizeof(struct msghdr));

    msg.msg_iov = &iov.data()[iovused];

    msgbytes = 0;
    STATS_MAX(this, msgused_high_watermark, msglist.size());

    return true;
}

bool Connection::addIov(const void* buf, size_t len) {

    size_t leftover;
    bool limit_to_mtu;

    if (len == 0) {
        return true;
    }

    do {
        struct msghdr* m = &msglist.back();

        /*
         * Limit the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = (1 == msglist.size());

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
            if (!addMsgHdr(false)) {
                return false;
            }
        }

        if (!ensureIovSpace()) {
            return false;
        }

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + msgbytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        // Update 'm' as we may have added an additional msghdr
        m = &msglist.back();

        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        msgbytes += len;
        ++iovused;
        STATS_MAX(this, iovused_high_watermark, getIovUsed());
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return true;
}

bool Connection::ensureIovSpace() {
    if (iovused < iov.size()) {
        // There is still size in the list
        return true;
    }

    // Try to double the size of the array
    try {
        iov.resize(iov.size() * 2);
    } catch (std::bad_alloc) {
        return false;
    }

    /* Point all the msghdr structures at the new list. */
    size_t ii;
    int iovnum;
    for (ii = 0, iovnum = 0; ii < msglist.size(); ii++) {
        msglist[ii].msg_iov = &iov[iovnum];
        iovnum += msglist[ii].msg_iovlen;
    }

    return true;
}

void Connection::maybeLogSlowCommand(
    const std::chrono::milliseconds& elapsed) const {

    std::chrono::milliseconds limit(500);

    switch (cmd) {
    case PROTOCOL_BINARY_CMD_COMPACT_DB:
        // We have no idea how slow this is, but just set a 30 minute
        // threshold for now to avoid it popping up in the logs all of
        // the times
        limit = std::chrono::milliseconds(1800 * 1000);
        break;
    case PROTOCOL_BINARY_CMD_SEQNO_PERSISTENCE:
        // I've heard that this may be slow as well...
        break;
    }

    if (elapsed > limit) {
        const char *opcode = memcached_opcode_2_text(cmd);
        char opcodetext[10];
        if (opcode == NULL) {
            snprintf(opcodetext, sizeof(opcodetext), "0x%0X", cmd);
            opcode = opcodetext;
        }
        LOG_WARNING(NULL, "%u: Slow %s operation on connection: %lu ms",
                    getId(), opcode, (unsigned long)elapsed.count());
    }
}

PipeConnection::PipeConnection() {
    peername = "pipe";
    sockname = "pipe";
}

PipeConnection::~PipeConnection() {
    if (settings.exit_on_connection_close) {
        exit(0);
    }
}
