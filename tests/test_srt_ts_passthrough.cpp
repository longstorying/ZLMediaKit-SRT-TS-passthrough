/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/macros.h"
#include "Network/Buffer.h"
#include "Network/Socket.h"
#include "Player/PlayerBase.h"
#include "Poller/EventPoller.h"
#include "Rtp/RtpMulticastOptions.h"
#include "Rtp/TSDecoder.h"
#include "Rtsp/Rtsp.h"
#include "TS/TSMediaSource.h"
#include "Util/mini.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#if defined(ENABLE_SRT) && defined(ENABLE_RTPPROXY)
#include "srt/SrtPlayerImp.h"
#endif

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

void expect(bool cond, const string &msg) {
    if (!cond) {
        throw runtime_error(msg);
    }
}

string makeTsPacket(uint8_t seed) {
    string packet(TS_PACKET_SIZE, static_cast<char>(seed));
    packet[0] = TS_SYNC_BYTE;
    return packet;
}

string makeTsPackets(uint8_t seed, size_t count) {
    string packets;
    packets.reserve(TS_PACKET_SIZE * count);
    for (size_t i = 0; i < count; ++i) {
        packets += makeTsPacket(static_cast<uint8_t>(seed + i));
    }
    return packets;
}

uint16_t readBe16(const string &data, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint8_t>(data[offset]) << 8) | static_cast<uint8_t>(data[offset + 1]));
}

uint32_t readBe32(const string &data, size_t offset) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) << 24)
           | (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 16)
           | (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 8)
           | static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3]));
}

#if defined(ENABLE_SRT) && defined(ENABLE_RTPPROXY)

sockaddr_storage makeUdpAddress(const char *host, uint16_t port) {
    sockaddr_storage addr;
    expect(SockUtil::getDomainIP(host, port, addr, AF_INET, SOCK_DGRAM, IPPROTO_UDP), "udp address should resolve");
    return addr;
}

uint8_t readMulticastTtl(int fd) {
#if defined(IP_MULTICAST_TTL)
    uint8_t ttl = 0;
#if defined(_WIN32)
    int len = sizeof(ttl);
#else
    socklen_t len = sizeof(ttl);
#endif
    expect(getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char *>(&ttl), &len) == 0, "read multicast ttl should succeed");
    return ttl;
#else
    (void)fd;
    return 0;
#endif
}

uint32_t readMulticastInterface(int fd) {
#if defined(IP_MULTICAST_IF)
    in_addr addr {};
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    expect(getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char *>(&addr), &len) == 0, "read multicast interface should succeed");
    return ntohl(addr.s_addr);
#else
    (void)fd;
    return 0;
#endif
}

void testSendRtpMulticastOptionsAreLoadedFromIni() {
    mINI ini;
    ini[kSendRtpOptionMulticastIf] = "127.0.0.1";
    ini[kSendRtpOptionMulticastTtl] = "32";

    MediaSourceEvent::SendRtpArgs args;
    loadSendRtpMulticastOptions(ini, args);

    expect(args.multicast_if == "127.0.0.1", "multicast_if should be loaded from ini options");
    expect(args.multicast_ttl == 32, "multicast_ttl should be loaded from ini options");
}

void testSendRtpMulticastOptionsRejectInvalidTtl() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto sock = Socket::createSocket(poller, false);
    expect(sock->bindUdpSock(0, "0.0.0.0", true), "udp socket should bind before applying multicast options");

    MediaSourceEvent::SendRtpArgs args;
    args.multicast_ttl = 300;
    auto addr = makeUdpAddress("239.255.0.1", 5004);

    bool threw = false;
    try {
        applySendRtpMulticastOptions(sock->rawFD(), addr, args);
    } catch (const exception &) {
        threw = true;
    }

    expect(threw, "multicast_ttl above 255 should be rejected before sending");
}

void testSendRtpMulticastOptionsApplyToUdpSocket() {
#if defined(IP_MULTICAST_TTL) && defined(IP_MULTICAST_IF)
    auto poller = EventPollerPool::Instance().getPoller();
    auto sock = Socket::createSocket(poller, false);
    expect(sock->bindUdpSock(0, "0.0.0.0", true), "udp socket should bind before applying multicast ttl and interface");

    MediaSourceEvent::SendRtpArgs args;
    args.multicast_if = "127.0.0.1";
    args.multicast_ttl = 32;
    auto addr = makeUdpAddress("239.255.0.1", 5004);

    applySendRtpMulticastOptions(sock->rawFD(), addr, args);

    expect(readMulticastTtl(sock->rawFD()) == 32, "multicast_ttl should be applied to the udp socket");
    expect(readMulticastInterface(sock->rawFD()) == ntohl(inet_addr("127.0.0.1")), "multicast_if should be applied to the udp socket");
#endif
}

class TestableSrtPlayerImp : public SrtPlayerImp {
public:
    using SrtPlayerImp::SrtPlayerImp;
    using SrtPlayerImp::inputTsPayloadForPassthrough;
};

class TestMediaSourceEvent : public MediaSourceEvent {
public:
    explicit TestMediaSourceEvent(EventPoller::Ptr poller) : _poller(std::move(poller)) {}

    int totalReaderCount(MediaSource &sender) override {
        return sender.readerCount();
    }

    EventPoller::Ptr getOwnerPoller(MediaSource &sender) override {
        return _poller;
    }

private:
    EventPoller::Ptr _poller;
};

void testSrtPayloadIsSplitIntoTsPackets() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    auto first = makeTsPacket(0x11);
    auto second = makeTsPacket(0x22);
    auto payload = first + second;

    expect(player->inputTsPayloadForPassthrough(payload.data(), payload.size()), "srt ts passthrough should consume payload when callback is set");
    expect(packets.size() == 1, "srt payload should batch complete TS packets from one input");
    expect(packets[0] == payload, "batched TS payload should be forwarded unchanged");

    player->setOnTsPacket(nullptr);
    expect(!player->inputTsPayloadForPassthrough(payload.data(), payload.size()), "passthrough should be disabled after callback is cleared");
}

void testSrtPayloadBatchesCompleteTsPacketsByRtpPayloadLimit() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    auto payload = makeTsPackets(0x30, 8);
    expect(player->inputTsPayloadForPassthrough(payload.data(), payload.size()), "multi TS payload should be consumed");
    expect(packets.size() == 2, "8 TS packets should be batched into 2 payload chunks with default RTP MTU");
    expect(packets[0] == payload.substr(0, 7 * TS_PACKET_SIZE), "first batched payload should fill the RTP TS payload limit");
    expect(packets[1] == payload.substr(7 * TS_PACKET_SIZE), "second batched payload should contain remaining TS packet");
}

void testSrtPayloadAcceptsEmptyInput() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    expect(player->inputTsPayloadForPassthrough(nullptr, 0), "empty SRT payload should be consumed without touching parser memory");
    expect(packets.empty(), "empty SRT payload should not emit TS packets");
    expect(player->inputTsPayloadForPassthrough(nullptr, TS_PACKET_SIZE), "null SRT payload should be consumed without touching parser memory");
    expect(packets.empty(), "null SRT payload should not emit TS packets");
}

void testSrtPayloadBuffersPartialTsPackets() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    auto packet = makeTsPacket(0x55);
    expect(player->inputTsPayloadForPassthrough(packet.data(), 100), "partial TS payload should be consumed for buffering");
    expect(packets.empty(), "partial TS payload should not be emitted before one complete TS packet");
    expect(player->inputTsPayloadForPassthrough(packet.data() + 100, packet.size() - 100), "remaining TS payload should be consumed");
    expect(packets.size() == 1, "buffered partial TS payload should emit exactly one packet after completion");
    expect(packets[0] == packet, "buffered TS packet should be forwarded unchanged");
}

void testSrtPayloadEmitsCompletePacketAndBuffersTrailingPartial() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    auto first = makeTsPacket(0x61);
    auto second = makeTsPacket(0x62);
    auto payload = first + second.substr(0, 80);

    expect(player->inputTsPayloadForPassthrough(payload.data(), payload.size()), "payload with trailing partial TS packet should be consumed");
    expect(packets.size() == 1, "complete TS packet before trailing partial should be emitted immediately");
    expect(packets[0] == first, "complete TS packet before trailing partial should be forwarded unchanged");

    expect(player->inputTsPayloadForPassthrough(second.data() + 80, second.size() - 80), "remaining trailing TS payload should be consumed");
    expect(packets.size() == 2, "trailing partial TS packet should be emitted after completion");
    expect(packets[1] == second, "completed trailing TS packet should be forwarded unchanged");
}

void testSrtCallbackResetClearsPartialTsPacket() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    auto collect_packet = [&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    };

    auto packet = makeTsPacket(0x63);
    player->setOnTsPacket(collect_packet);
    expect(player->inputTsPayloadForPassthrough(packet.data(), 100), "partial TS payload should be consumed before callback reset");
    expect(packets.empty(), "partial TS payload should not be emitted before callback reset");

    player->setOnTsPacket(nullptr);
    player->setOnTsPacket(collect_packet);
    expect(player->inputTsPayloadForPassthrough(packet.data() + 100, packet.size() - 100), "old trailing TS payload should be consumed after callback reset");
    expect(packets.empty(), "callback reset should clear buffered partial TS payload");

    expect(player->inputTsPayloadForPassthrough(packet.data(), packet.size()), "valid TS packet after callback reset should be consumed");
    expect(packets.size() == 1, "valid TS packet after callback reset should be emitted");
    expect(packets[0] == packet, "valid TS packet after callback reset should be forwarded unchanged");
}

void testSrtInvalidPayloadIsDroppedAndNextTsPacketStillWorks() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = std::make_shared<TestableSrtPlayerImp>(poller);

    vector<string> packets;
    player->setOnTsPacket([&](const Buffer::Ptr &packet) {
        packets.emplace_back(packet->data(), packet->size());
    });

    string invalid_payload(64, '\x12');
    expect(player->inputTsPayloadForPassthrough(invalid_payload.data(), invalid_payload.size()), "invalid SRT payload should be consumed by passthrough path");
    expect(packets.empty(), "invalid SRT payload should not emit TS packets");

    auto packet = makeTsPacket(0x66);
    expect(player->inputTsPayloadForPassthrough(packet.data(), packet.size()), "valid TS packet after invalid payload should be consumed");
    expect(packets.size() == 1, "valid TS packet after parser reset should be emitted");
    expect(packets[0] == packet, "valid TS packet after parser reset should be forwarded unchanged");
}

void testPlayerFactoryKeepsPassthroughScopedToSrt() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto srt_player = PlayerBase::createPlayer(poller, "srt://127.0.0.1:10080?streamid=#!::r=live/test,m=request");
    auto rtmp_player = PlayerBase::createPlayer(poller, "rtmp://127.0.0.1/live/test");

    expect(std::dynamic_pointer_cast<SrtPlayerImp>(srt_player) != nullptr, "srt:// URL should create SrtPlayerImp");
    expect(std::dynamic_pointer_cast<SrtPlayerImp>(rtmp_player) == nullptr, "rtmp:// URL should not create SrtPlayerImp");
}

void testTsMediaSourceSendsRawTsAsRtpMp2t() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto receiver = Socket::createSocket(poller, false);
    expect(receiver->bindUdpSock(0, "127.0.0.1", true), "udp receiver should bind to loopback");

    mutex mtx;
    condition_variable cv;
    vector<string> received_packets;
    receiver->setOnRead([&](const Buffer::Ptr &buf, struct sockaddr *, int) {
        lock_guard<mutex> lock(mtx);
        received_packets.emplace_back(buf->data(), buf->size());
        cv.notify_one();
    });

    MediaTuple tuple = {DEFAULT_VHOST, "test", "srt_ts_passthrough", ""};
    auto src = std::make_shared<TSMediaSource>(tuple, 1);
    auto listener = std::make_shared<TestMediaSourceEvent>(poller);
    src->setListener(listener);

    auto priming_packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(0x33)));
    priming_packet->time_stamp = 0;
    src->onWrite(std::move(priming_packet), true);

    bool start_done = false;
    SockException start_error;
    MediaSourceEvent::SendRtpArgs args;
    args.data_type = MediaSourceEvent::SendRtpArgs::kRtpTS;
    args.con_type = MediaSourceEvent::SendRtpArgs::kUdpActive;
    args.dst_url = "127.0.0.1";
    args.dst_port = receiver->get_local_port();
    args.ssrc = "12345678";

    src->startSendRtp(args, [&](uint16_t, const SockException &ex) {
        lock_guard<mutex> lock(mtx);
        start_error = ex;
        start_done = true;
        cv.notify_one();
    });

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return start_done; }), "rtp sender should start");
        expect(!start_error, string("rtp sender should start without error: ") + start_error.what());
    }

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return !received_packets.empty(); }), "receiver should get priming RTP packet");
        received_packets.clear();
    }

    for (auto seed : {0x44, 0x45}) {
        auto packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(static_cast<uint8_t>(seed))));
        packet->time_stamp = 33;
        src->onWrite(std::move(packet), true);
    }

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return received_packets.size() >= 2; }), "receiver should get RTP packets");
    }

    const auto &first = received_packets[0];
    const auto &second = received_packets[1];
    const auto *rtp = reinterpret_cast<const uint8_t *>(first.data());
    expect(first.size() >= RtpPacket::kRtpHeaderSize + TS_PACKET_SIZE, "received RTP packet should contain one TS payload");
    expect((rtp[0] >> 6) == RtpPacket::kRtpVersion, "RTP version should be 2");
    expect((rtp[1] & 0x7F) == Rtsp::PT_MP2T, "RTP payload type should be static MP2T/PT=33");
    expect(readBe32(first, 8) == 12345678, "RTP SSRC should match configured value");
    expect(first.size() - RtpPacket::kRtpHeaderSize == TS_PACKET_SIZE, "first RTP payload should contain one TS packet for one-packet input");
    expect(second.size() - RtpPacket::kRtpHeaderSize == TS_PACKET_SIZE, "second RTP payload should contain one TS packet for one-packet input");
    expect((first.size() - RtpPacket::kRtpHeaderSize) % TS_PACKET_SIZE == 0, "RTP payload length should be aligned to TS packet size");
    expect((second.size() - RtpPacket::kRtpHeaderSize) % TS_PACKET_SIZE == 0, "second RTP payload length should be aligned to TS packet size");
    expect(readBe16(second, 2) == static_cast<uint16_t>(readBe16(first, 2) + 1), "RTP sequence should increase by one");
    expect(rtp[RtpPacket::kRtpHeaderSize] == TS_SYNC_BYTE, "RTP payload should start with TS sync byte");
    expect(src->stopSendRtp(args.ssrc), "stopSendRtp should stop the active sender");
}

void testTsMediaSourceSplitsOversizedTsBufferOnTsBoundary() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto receiver = Socket::createSocket(poller, false);
    expect(receiver->bindUdpSock(0, "127.0.0.1", true), "udp receiver should bind to loopback for oversized TS test");

    mutex mtx;
    condition_variable cv;
    vector<string> received_packets;
    receiver->setOnRead([&](const Buffer::Ptr &buf, struct sockaddr *, int) {
        lock_guard<mutex> lock(mtx);
        received_packets.emplace_back(buf->data(), buf->size());
        cv.notify_one();
    });

    MediaTuple tuple = {DEFAULT_VHOST, "test", "srt_ts_oversized_payload", ""};
    auto src = std::make_shared<TSMediaSource>(tuple, 1);
    auto listener = std::make_shared<TestMediaSourceEvent>(poller);
    src->setListener(listener);

    auto priming_packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(0x50)));
    priming_packet->time_stamp = 0;
    src->onWrite(std::move(priming_packet), true);

    bool start_done = false;
    SockException start_error;
    MediaSourceEvent::SendRtpArgs args;
    args.data_type = MediaSourceEvent::SendRtpArgs::kRtpTS;
    args.con_type = MediaSourceEvent::SendRtpArgs::kUdpActive;
    args.dst_url = "127.0.0.1";
    args.dst_port = receiver->get_local_port();
    args.ssrc = "45678901";

    src->startSendRtp(args, [&](uint16_t, const SockException &ex) {
        lock_guard<mutex> lock(mtx);
        start_error = ex;
        start_done = true;
        cv.notify_one();
    });

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return start_done; }), "rtp sender should start for oversized TS test");
        expect(!start_error, string("rtp sender should start without error for oversized TS test: ") + start_error.what());
    }

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return !received_packets.empty(); }), "receiver should get priming RTP packet for oversized TS test");
        received_packets.clear();
    }

    auto packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPackets(0x60, 8)));
    packet->time_stamp = 40;
    src->onWrite(std::move(packet), true);

    {
        unique_lock<mutex> lock(mtx);
        expect(cv.wait_for(lock, chrono::seconds(3), [&] { return received_packets.size() >= 2; }), "oversized TS buffer should be split into RTP packets");
    }

    auto first_payload_size = received_packets[0].size() - RtpPacket::kRtpHeaderSize;
    auto second_payload_size = received_packets[1].size() - RtpPacket::kRtpHeaderSize;
    expect(first_payload_size == 7 * TS_PACKET_SIZE, "first RTP payload should carry 7 TS packets under default MTU");
    expect(second_payload_size == TS_PACKET_SIZE, "second RTP payload should carry the remaining TS packet");
    expect(received_packets[0][RtpPacket::kRtpHeaderSize] == TS_SYNC_BYTE, "first split RTP payload should start with TS sync byte");
    expect(received_packets[1][RtpPacket::kRtpHeaderSize] == TS_SYNC_BYTE, "second split RTP payload should start with TS sync byte");
    expect(src->stopSendRtp(args.ssrc), "stopSendRtp should stop oversized TS sender");
}

void testTsMediaSourceRejectsUnsupportedRtpOptions() {
    auto poller = EventPollerPool::Instance().getPoller();
    MediaTuple tuple = {DEFAULT_VHOST, "test", "srt_ts_reject_options", ""};
    auto src = std::make_shared<TSMediaSource>(tuple, 1);
    auto listener = std::make_shared<TestMediaSourceEvent>(poller);
    src->setListener(listener);

    bool base_done = false;
    SockException base_error;
    MediaSourceEvent::SendRtpArgs ps_args;
    ps_args.data_type = MediaSourceEvent::SendRtpArgs::kRtpPS;
    ps_args.con_type = MediaSourceEvent::SendRtpArgs::kUdpActive;
    ps_args.dst_url = "127.0.0.1";
    ps_args.dst_port = 12345;
    std::static_pointer_cast<MediaSource>(src)->startSendRtp(ps_args, [&](uint16_t, const SockException &ex) {
        base_error = ex;
        base_done = true;
    });
    expect(base_done, "TS MediaSource should reject PS RTP request synchronously");
    expect(base_error, "TS MediaSource should reject non-TS RTP output");

    auto priming_packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(0x77)));
    priming_packet->time_stamp = 0;
    src->onWrite(std::move(priming_packet), true);

    bool con_done = false;
    SockException con_error;
    MediaSourceEvent::SendRtpArgs tcp_args;
    tcp_args.data_type = MediaSourceEvent::SendRtpArgs::kRtpTS;
    tcp_args.con_type = MediaSourceEvent::SendRtpArgs::kTcpActive;
    tcp_args.dst_url = "127.0.0.1";
    tcp_args.dst_port = 12345;
    tcp_args.ssrc = "22334455";
    src->startSendRtp(tcp_args, [&](uint16_t, const SockException &ex) {
        con_error = ex;
        con_done = true;
    });
    expect(con_done, "TS passthrough RTP sender should reject unsupported connection type synchronously");
    expect(con_error, "TS passthrough RTP sender should reject non-UDP-active output");
}

void testTsMediaSourceDirectStartRejectsNonTsRtpOutput() {
    auto poller = EventPollerPool::Instance().getPoller();
    MediaTuple tuple = {DEFAULT_VHOST, "test", "srt_ts_direct_reject_non_ts", ""};
    auto src = std::make_shared<TSMediaSource>(tuple, 1);
    auto listener = std::make_shared<TestMediaSourceEvent>(poller);
    src->setListener(listener);

    auto priming_packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(0x88)));
    priming_packet->time_stamp = 0;
    src->onWrite(std::move(priming_packet), true);

    bool done = false;
    SockException error;
    MediaSourceEvent::SendRtpArgs args;
    args.data_type = MediaSourceEvent::SendRtpArgs::kRtpPS;
    args.con_type = MediaSourceEvent::SendRtpArgs::kUdpActive;
    args.dst_url = "127.0.0.1";
    args.dst_port = 12345;
    args.ssrc = "33445566";
    src->startSendRtp(args, [&](uint16_t, const SockException &ex) {
        error = ex;
        done = true;
    });

    expect(done, "direct TS MediaSource startSendRtp should reject non-TS output synchronously");
    expect(error, "direct TS MediaSource startSendRtp should reject PS RTP output");
}

void testTsMediaSourceStartWithoutListenerReturnsCallbackError() {
    MediaTuple tuple = {DEFAULT_VHOST, "test", "srt_ts_no_listener", ""};
    auto src = std::make_shared<TSMediaSource>(tuple, 1);

    auto priming_packet = std::make_shared<TSPacket>(std::make_shared<BufferString>(makeTsPacket(0x90)));
    priming_packet->time_stamp = 0;
    src->onWrite(std::move(priming_packet), true);

    bool done = false;
    bool threw = false;
    SockException error;
    MediaSourceEvent::SendRtpArgs args;
    args.data_type = MediaSourceEvent::SendRtpArgs::kRtpTS;
    args.con_type = MediaSourceEvent::SendRtpArgs::kUdpActive;
    args.dst_url = "127.0.0.1";
    args.dst_port = 12345;
    args.ssrc = "55667788";

    try {
        src->startSendRtp(args, [&](uint16_t, const SockException &ex) {
            error = ex;
            done = true;
        });
    } catch (const exception &) {
        threw = true;
    }

    expect(!threw, "TS MediaSource startSendRtp should report missing listener via callback instead of throwing");
    expect(done, "TS MediaSource startSendRtp should invoke callback when listener is missing");
    expect(error, "TS MediaSource startSendRtp should return error when listener is missing");
}

#endif

} // namespace

int main() {
#if defined(ENABLE_SRT) && defined(ENABLE_RTPPROXY)
    try {
        testSrtPayloadIsSplitIntoTsPackets();
        testSrtPayloadBatchesCompleteTsPacketsByRtpPayloadLimit();
        testSrtPayloadAcceptsEmptyInput();
        testSrtPayloadBuffersPartialTsPackets();
        testSrtPayloadEmitsCompletePacketAndBuffersTrailingPartial();
        testSrtCallbackResetClearsPartialTsPacket();
        testSrtInvalidPayloadIsDroppedAndNextTsPacketStillWorks();
        testPlayerFactoryKeepsPassthroughScopedToSrt();
        testSendRtpMulticastOptionsAreLoadedFromIni();
        testSendRtpMulticastOptionsRejectInvalidTtl();
        testSendRtpMulticastOptionsApplyToUdpSocket();
        testTsMediaSourceSendsRawTsAsRtpMp2t();
        testTsMediaSourceSplitsOversizedTsBufferOnTsBoundary();
        testTsMediaSourceRejectsUnsupportedRtpOptions();
        testTsMediaSourceDirectStartRejectsNonTsRtpOutput();
        testTsMediaSourceStartWithoutListenerReturnsCallbackError();
        cout << "test_srt_ts_passthrough passed" << endl;
        return EXIT_SUCCESS;
    } catch (const exception &ex) {
        cerr << "test_srt_ts_passthrough failed: " << ex.what() << endl;
        return EXIT_FAILURE;
    }
#else
    cout << "test_srt_ts_passthrough skipped: ENABLE_SRT and ENABLE_RTPPROXY are required" << endl;
    return EXIT_SUCCESS;
#endif
}
