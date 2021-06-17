/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2020 John
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <srs_app_quic_io_loop.hpp>

using namespace std;

#include <srs_app_config.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_log.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_utility.hpp>
#include <srs_app_pithy_print.hpp>
#include <srs_core_autofree.hpp>
#include <srs_app_quic_conn.hpp>
#include <srs_app_quic_tls.hpp>
#include <srs_app_quic_util.hpp>
#include <srs_app_server.hpp>
#include <srs_service_utility.hpp>
#include <srs_protocol_utility.hpp>
#include <srs_app_rtc_forward.hpp>

SrsQuicListener::SrsQuicListener(ISrsQuicHandler* handler, SrsQuicListenerType type)
{
    handler_ = handler;
    listen_type_ = type;
}

SrsQuicListener::~SrsQuicListener()
{
    srs_freep(handler_);
}

srs_error_t SrsQuicListener::listen(const string& ip, int port)
{
    srs_error_t err = srs_success;

    listen_sa_.sin_family = AF_INET;
    listen_sa_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &listen_sa_.sin_addr) != 1) {
        return srs_error_new(ERROR_QUIC_SERVER, "invalid addr=%s", ip.c_str());
    }

    SrsUdpMuxListener* listener = new SrsUdpMuxListener(this, ip, port, "QUIC");
    if ((err = listener->listen()) != srs_success) {
        srs_freep(listener);
        return srs_error_wrap(err, "listen %s:%d", ip.c_str(), port);
    }

    srs_trace("quic listen at udp://%s:%d, fd=%d", ip.c_str(), port, listener->fd());

    return err;
}

std::string SrsQuicListener::get_key()
{
    if (listen_type_ == SrsQuicListenerRtcForward) {
        return _srs_config->get_rtc_server_quic_ssl_key();
    } else if (listen_type_ == SrsQuicListenerHttpApi) {
        return _srs_config->get_http_api_quic_ssl_key();
    } else if (listen_type_ == SrsQuicListenerHttpStream) {
        return _srs_config->get_http_stream_quic_ssl_key();
    }

    return "";
}

std::string SrsQuicListener::get_cert()
{
    if (listen_type_ == SrsQuicListenerRtcForward) {
        return _srs_config->get_rtc_server_quic_ssl_cert();
    } else if (listen_type_ == SrsQuicListenerHttpApi) {
        return _srs_config->get_http_api_quic_ssl_cert();
    } else if (listen_type_ == SrsQuicListenerHttpStream) {
        return _srs_config->get_http_stream_quic_ssl_cert();
    }

    return "";
}

srs_error_t SrsQuicListener::on_udp_packet(SrsUdpMuxSocket* skt)
{
    return _quic_io_loop->on_udp_packet(skt, this);
}

srs_error_t SrsQuicListener::on_accept_quic_conn(SrsQuicConnection* quic_conn)
{
    return handler_->on_quic_client(quic_conn, listen_type_);
}

SrsQuicIoLoop::SrsQuicIoLoop()
{
    quic_conn_map_ = new SrsResourceManager("quic conn map", true/*verbose*/);
}

SrsQuicIoLoop::~SrsQuicIoLoop()
{
    srs_freep(quic_conn_map_);
}

srs_error_t SrsQuicIoLoop::initialize()
{
    srs_error_t err = srs_success;
    return err;
}

void SrsQuicIoLoop::subscribe(SrsQuicConnection* quic_conn)
{
    srs_trace("subscribe quic conn %s", quic_conn->get_conn_name().c_str());
    quic_conn_map_->subscribe(quic_conn);
}

void SrsQuicIoLoop::unsubscribe(SrsQuicConnection* quic_conn)
{
    srs_trace("unsubscribe quic conn %s", quic_conn->get_conn_name().c_str());
    quic_conn_map_->unsubscribe(quic_conn);
}

void SrsQuicIoLoop::remove(ISrsResource* resource)
{
    SrsQuicConnection* quic_conn = dynamic_cast<SrsQuicConnection*>(resource);
    srs_trace("remove quic conn %s", quic_conn->get_conn_name().c_str());
    quic_conn_map_->remove(resource);
}

srs_error_t SrsQuicIoLoop::on_udp_packet(SrsUdpMuxSocket* skt, SrsQuicListener* listener)
{
    srs_error_t err = srs_success;

    uint8_t* data = reinterpret_cast<uint8_t*>(skt->data()); 
    int size = skt->size();

    uint32_t version = UINT32_MAX;

    const uint8_t *dcid = NULL;
    const uint8_t *scid = NULL;
    size_t dcid_len = 0;
    size_t scid_len = 0;

    int ret = ngtcp2_pkt_decode_version_cid(&version, &dcid, &dcid_len, &scid, &scid_len, 
                                            data, size, kServerCidLen);
    if (ret != 0) {
        if (ret == 1) {
            return send_version_negotiation(skt, version, dcid, dcid_len, scid, scid_len);
        } else {
            return srs_error_new(ERROR_QUIC_UDP, "invalid/unsupport quic udp packet");
        }
    }

    srs_verbose("scid=%s, dcid=%s", quic_conn_id_dump(scid, scid_len).c_str(),
        quic_conn_id_dump(dcid, dcid_len).c_str());

    SrsQuicConnection* quic_conn = NULL;
    string connid(reinterpret_cast<const char*>(dcid), dcid_len);
    ISrsResource* conn = quic_conn_map_->find_by_name(connid);
    quic_conn = dynamic_cast<SrsQuicConnection*>(conn);
    if (quic_conn) {
        // Switch to the quic_conn to write logs to the context.
        quic_conn->switch_to_context();
    } else {
        if (conn) {
            return srs_error_new(ERROR_QUIC_CONN, "maybe duplicated conn %s", 
                quic_conn_id_dump(dcid, dcid_len).c_str());
        }
        // TODO: FIXME: 
        // It maybe no a new connection,  when server side handshake loss and client 
        // retry connect can occru, have not implement this case.
        if ((err = new_connection(skt, listener, &quic_conn)) != srs_success) {
            return srs_error_wrap(err, "create new quic connection failed");
        }
    }

    return quic_conn->on_udp_packet(skt, data, size);
}

srs_error_t SrsQuicIoLoop::send_version_negotiation(SrsUdpMuxSocket* skt, const uint8_t version, 
    const uint8_t* dcid, const size_t dcid_len, const uint8_t* scid, const size_t scid_len)
{
    srs_error_t err = srs_success;

    vector<uint32_t> server_versions;
    server_versions.push_back(generate_reserved_version(reinterpret_cast<const sockaddr*>(skt->peer_addr()), 
        skt->peer_addrlen(), version));
    server_versions.push_back(NGTCP2_PROTO_VER_V1);

    for (uint32_t v = NGTCP2_PROTO_VER_MIN; v <= NGTCP2_PROTO_VER_MAX; ++v) {
        server_versions.push_back(v);
    }

    char buf[NGTCP2_MAX_PKTLEN_IPV4];
    int nb = ngtcp2_pkt_write_version_negotiation(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 
        (uint8_t)(random() % 256), dcid, dcid_len, scid, scid_len, server_versions.data(), server_versions.size());
    if (nb < 0) {
        return srs_error_new(ERROR_QUIC_CONN, "version negotiation failed, ret=%d", nb);
    }

    if ((err = skt->sendto(buf, nb, 0)) != srs_success) {
        return srs_error_wrap(err, "send quic version negotiation");
    }

    return err;
}


srs_error_t SrsQuicIoLoop::new_connection(SrsUdpMuxSocket* skt, SrsQuicListener* listener, SrsQuicConnection** p_conn)
{
    srs_error_t err = srs_success;

    uint8_t* data = reinterpret_cast<uint8_t*>(skt->data()); 
    int size = skt->size();
    ngtcp2_pkt_hd hd;

    int ret = ngtcp2_accept(&hd, data, size);
    if (ret == -1) {
        return srs_error_new(ERROR_QUIC_CONN, "accept failed, ret=%d(%s)", ret, ngtcp2_strerror(ret));
    } else if (ret == 1) {
        srs_warn("quic client version=%u, server version %u-%u, need negotation", 
            hd.version, NGTCP2_PROTO_VER_MIN, NGTCP2_PROTO_VER_MAX);
        return send_version_negotiation(skt, hd.version, hd.scid.data, hd.scid.datalen, hd.dcid.data, hd.dcid.datalen);
    }

    switch (hd.type) {
        case NGTCP2_PKT_INITIAL: {
            // TODO: FIXME: check token
        } break;
        case NGTCP2_PKT_0RTT: {
            // TODO: FIXME: process 0rtt packet.
            srs_warn("quic 0rtt packet");
        } break;
        default: {
        } break;
    }

    SrsContextId cid = _srs_context->get_id();
    SrsQuicConnection* quic_conn = new SrsQuicConnection(listener, cid);
    if ((err = quic_conn->accept(skt, &hd)) != srs_success) {
        srs_freep(quic_conn);
        return srs_error_wrap(err, "quic connect init failed");
    }

    // Accept quic conn, and start state-thread run cycle of this quic conn.
    if ((err = listener->on_accept_quic_conn(quic_conn)) != srs_success) {
        srs_freep(quic_conn);
        return srs_error_wrap(err, "on quic client failed");
    }

    string conn_id = quic_conn->get_scid();
    srs_trace("add new quic connection=%s", quic_conn_id_dump(conn_id).c_str());
    quic_conn_map_->add_with_name(conn_id, quic_conn);
    *p_conn = quic_conn;

    return err;
}

SrsQuicIoLoop* _quic_io_loop = new SrsQuicIoLoop();
