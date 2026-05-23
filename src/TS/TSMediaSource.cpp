/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_RTPPROXY)

#include "TSMediaSource.h"
#include "Common/config.h"
#include "Network/sockutil.h"
#include "Rtp/RtpMulticastOptions.h"
#include "Rtp/TSDecoder.h"
#include "Rtsp/RtpCodec.h"
#include "Rtsp/Rtsp.h"
#include "Rtsp/RtspSession.h"
#include "Thread/WorkThreadPool.h"
#include "Util/logger.h"
#include "Util/uv_errno.h"
#include <algorithm>
#include <cstdlib>

using namespace std;
using namespace toolkit;

namespace mediakit {

class TSMediaSourceRtpSender : public enable_shared_from_this<TSMediaSourceRtpSender> {
public:
    explicit TSMediaSourceRtpSender(EventPoller::Ptr poller) {
        _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();
        _socket_rtp = Socket::createSocket(_poller, false);
    }

    void setOnClose(function<void(const SockException &ex)> on_close) {
        _on_close = std::move(on_close);
    }

    void startSend(const MediaSourceEvent::SendRtpArgs &args, const function<void(uint16_t, const SockException &ex)> &cb) {
        _args = args;
        if (_args.con_type != MediaSourceEvent::SendRtpArgs::kUdpActive) {
            cb(0, SockException(Err_other, "ts media source passthrough only supports udp active rtp"));
            return;
        }

        auto ssrc = _args.ssrc.empty() ? 0 : static_cast<uint32_t>(strtoul(_args.ssrc.data(), nullptr, 10));
        _rtp_info = std::make_shared<RtpInfo>(ssrc, getRtpMtuSize(), 90000, static_cast<uint8_t>(Rtsp::PT_MP2T), 0, 0);

        auto poller = _poller;
        weak_ptr<TSMediaSourceRtpSender> weak_self = shared_from_this();
        WorkThreadPool::Instance().getPoller()->async([cb, args, weak_self, poller]() {
            struct sockaddr_storage addr;
            if (!SockUtil::getDomainIP(args.dst_url.data(), args.dst_port, addr, AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
                poller->async([args, cb]() {
                    cb(0, SockException(Err_dns, StrPrinter << "dns resolution failed: " << args.dst_url));
                });
                return;
            }

            poller->async([args, addr, weak_self, cb]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                string ifr_ip = addr.ss_family == AF_INET ? "0.0.0.0" : "::";
                try {
                    if (args.src_port) {
                        if (!strong_self->_socket_rtp->bindUdpSock(args.src_port, ifr_ip, true)) {
                            throw invalid_argument(StrPrinter << "open udp active client failed on port: "
                                                              << args.src_port << ", err: " << get_uv_errmsg(true));
                        }
                    } else {
                        auto pr = make_pair(strong_self->_socket_rtp, Socket::createSocket(strong_self->_poller, false));
                        makeSockPair(pr, ifr_ip, true, true);
                    }
                    applySendRtpMulticastOptions(strong_self->_socket_rtp->rawFD(), addr, args);
                } catch (std::exception &ex) {
                    cb(0, SockException(Err_other, ex.what()));
                    return;
                }
                strong_self->_socket_rtp->bindPeerAddr((struct sockaddr *) &addr, 0, true);
                strong_self->onConnect();
                cb(strong_self->_socket_rtp->get_local_port(), SockException());
            });
        });
        InfoL << "start udp active send raw ts rtp to: " << args.dst_url << ":" << args.dst_port;
    }

    void inputTs(const TSMediaSource::RingDataType &ts_list) {
        if (!_is_connect || !_rtp_info || !ts_list) {
            return;
        }

        vector<Buffer::Ptr> rtp_list;
        string payload;
        payload.reserve(_rtp_info->getMaxSize());
        uint64_t payload_stamp = _stamp_ticker.elapsedTime();

        ts_list->for_each([&](const TSPacket::Ptr &ts) {
            if (!ts || !ts->size()) {
                return;
            }
            appendTsPayload(rtp_list, payload, payload_stamp, ts->data(), ts->size(), ts->time_stamp);
        });
        flushPayload(rtp_list, payload, payload_stamp, true);
        sendRtpList(rtp_list);
    }

private:
    static size_t getRtpMtuSize() {
        GET_CONFIG(uint32_t, video_mtu, Rtp::kVideoMtuSize);
        auto payload_size = video_mtu > RtpPacket::kRtpHeaderSize ? video_mtu - RtpPacket::kRtpHeaderSize : 0;
        payload_size -= payload_size % TS_PACKET_SIZE;
        payload_size = std::max<size_t>(payload_size, TS_PACKET_SIZE);
        return RtpPacket::kRtpHeaderSize + payload_size;
    }

    void appendTsPayload(vector<Buffer::Ptr> &rtp_list, string &payload, uint64_t &payload_stamp,
                         const char *data, size_t len, uint64_t stamp) {
        if (!data || !len) {
            if (len) {
                WarnL << "drop null ts payload, len:" << len;
            }
            return;
        }

        auto complete_len = len - (len % TS_PACKET_SIZE);
        if (complete_len != len) {
            WarnL << "drop incomplete ts payload tail, len:" << len;
        }

        size_t offset = 0;
        auto max_payload_size = _rtp_info->getMaxSize();
        while (offset < complete_len) {
            if (payload.size() >= max_payload_size) {
                flushPayload(rtp_list, payload, payload_stamp, false);
            }
            if (payload.empty()) {
                payload_stamp = stamp;
            }

            auto append_size = std::min(max_payload_size - payload.size(), complete_len - offset);
            append_size -= append_size % TS_PACKET_SIZE;
            if (!append_size) {
                flushPayload(rtp_list, payload, payload_stamp, false);
                continue;
            }

            payload.append(data + offset, append_size);
            payload_stamp = stamp;
            offset += append_size;
        }
    }

    void flushPayload(vector<Buffer::Ptr> &rtp_list, string &payload, uint64_t stamp, bool mark) {
        if (payload.empty()) {
            return;
        }
        rtp_list.emplace_back(_rtp_info->makeRtp(TrackVideo, payload.data(), payload.size(), mark, stamp));
        payload.clear();
    }

    void sendRtpList(vector<Buffer::Ptr> &rtp_list) {
        size_t index = 0;
        auto size = rtp_list.size();
        for (auto &packet : rtp_list) {
            _socket_rtp->send(std::make_shared<BufferRtp>(std::move(packet), RtpPacket::kRtpTcpHeaderSize), nullptr, 0, ++index == size);
        }
    }

    void onConnect() {
        _is_connect = true;
        SockUtil::setSendBuf(_socket_rtp->rawFD(), 4 * 1024 * 1024);
        weak_ptr<TSMediaSourceRtpSender> weak_self = shared_from_this();
        _socket_rtp->setOnRead(nullptr);
        _socket_rtp->setOnErr([weak_self](const SockException &err) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onErr(err);
        });
        InfoL << "startSend raw ts rtp success: " << _socket_rtp->get_peer_ip() << ":" << _socket_rtp->get_peer_port()
              << ", data_type: " << _args.data_type << ", con_type: " << _args.con_type;
    }

    void onErr(const SockException &ex) {
        _is_connect = false;
        WarnL << "send raw ts rtp connection lost: " << ex;
        auto cb = _on_close;
        if (cb) {
            _poller->async([cb, ex]() { cb(ex); }, false);
        }
    }

private:
    bool _is_connect = false;
    EventPoller::Ptr _poller;
    Socket::Ptr _socket_rtp;
    RtpInfo::Ptr _rtp_info;
    MediaSourceEvent::SendRtpArgs _args;
    Ticker _stamp_ticker;
    function<void(const SockException &ex)> _on_close;
};

void TSMediaSource::startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const function<void(uint16_t, const SockException &)> cb) {
    if (args.data_type != MediaSourceEvent::SendRtpArgs::kRtpTS) {
        cb(0, SockException(Err_other, "ts media source only supports rtp ts output"));
        return;
    }

    auto ring = _ring;
    if (!ring) {
        cb(0, SockException(Err_other, "ts media source ring is not ready"));
        return;
    }

    auto ssrc = args.ssrc;
    auto ssrc_multi_send = args.ssrc_multi_send;
    EventPoller::Ptr poller;
    try {
        poller = getOwnerPoller();
    } catch (std::exception &ex) {
        cb(0, SockException(Err_other, ex.what()));
        return;
    }

    auto rtp_sender = std::make_shared<TSMediaSourceRtpSender>(poller);
    weak_ptr<TSMediaSource> weak_self = static_pointer_cast<TSMediaSource>(shared_from_this());

    rtp_sender->setOnClose([weak_self, ssrc](const SockException &ex) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->getOwnerPoller()->async([strong_self, ssrc, ex]() {
            WarnL << "stream:" << strong_self->getUrl() << " stop send raw ts rtp:" << ssrc << ", reason:" << ex;
            strong_self->_rtp_sender.erase(ssrc);
        });
    });

    rtp_sender->startSend(args, [ssrc, ssrc_multi_send, weak_self, rtp_sender, cb, ring, poller](uint16_t local_port, const SockException &ex) mutable {
        cb(local_port, ex);
        auto strong_self = weak_self.lock();
        if (!strong_self || ex) {
            return;
        }

        auto reader = ring->attach(poller);
        reader->setReadCB([rtp_sender](const TSMediaSource::RingDataType &ts_list) { rtp_sender->inputTs(ts_list); });

        strong_self->getOwnerPoller()->async([=]() {
            if (!ssrc_multi_send) {
                strong_self->_rtp_sender.erase(ssrc);
            }
            weak_ptr<TSMediaSourceRtpSender> sender = rtp_sender;
            strong_self->_rtp_sender.emplace(ssrc, make_tuple(reader, sender));
        });
    });
}

bool TSMediaSource::stopSendRtp(const string &ssrc) {
    if (ssrc.empty()) {
        auto size = _rtp_sender.size();
        _rtp_sender.clear();
        return size;
    }
    return _rtp_sender.erase(ssrc);
}

} // namespace mediakit

#endif // defined(ENABLE_RTPPROXY)
