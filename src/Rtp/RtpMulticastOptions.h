/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTPMULTICASTOPTIONS_H
#define ZLMEDIAKIT_RTPMULTICASTOPTIONS_H

#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "Common/MediaSource.h"
#include "Network/sockutil.h"
#include "Util/mini.h"
#include "Util/util.h"
#include "Util/uv_errno.h"

namespace mediakit {

static const char *kSendRtpOptionMulticastIf = "multicast_if";
static const char *kSendRtpOptionMulticastTtl = "multicast_ttl";

inline bool isSendRtpIpv4MulticastAddress(const sockaddr_storage &addr) {
    if (addr.ss_family != AF_INET) {
        return false;
    }
    auto ipv4 = ntohl(reinterpret_cast<const sockaddr_in *>(&addr)->sin_addr.s_addr);
    return ipv4 >= 0xE0000000UL && ipv4 <= 0xEFFFFFFFUL;
}

inline void validateSendRtpMulticastOptions(const MediaSourceEvent::SendRtpArgs &args) {
    if (args.multicast_ttl < -1 || args.multicast_ttl > 255) {
        throw std::invalid_argument(StrPrinter << "invalid multicast_ttl: " << args.multicast_ttl);
    }
    if (!args.multicast_if.empty() && !toolkit::SockUtil::is_ipv4(args.multicast_if.data())) {
        throw std::invalid_argument(StrPrinter << "invalid multicast_if: " << args.multicast_if);
    }
}

inline void loadSendRtpMulticastOptions(const toolkit::mINI &ini, MediaSourceEvent::SendRtpArgs &args) {
    auto &options = const_cast<toolkit::mINI &>(ini);
    auto multicast_if = options[kSendRtpOptionMulticastIf];
    if (!multicast_if.empty()) {
        args.multicast_if = multicast_if;
    }

    auto multicast_ttl = options[kSendRtpOptionMulticastTtl];
    if (!multicast_ttl.empty()) {
        args.multicast_ttl = multicast_ttl.as<int>();
    }
}

inline void applySendRtpMulticastOptions(int fd, const sockaddr_storage &dst_addr, const MediaSourceEvent::SendRtpArgs &args) {
    validateSendRtpMulticastOptions(args);
    if (!isSendRtpIpv4MulticastAddress(dst_addr)) {
        return;
    }

    if (args.multicast_ttl >= 0 && toolkit::SockUtil::setMultiTTL(fd, static_cast<uint8_t>(args.multicast_ttl)) != 0) {
        throw std::invalid_argument(StrPrinter << "set multicast_ttl failed: " << args.multicast_ttl << ", err: " << toolkit::get_uv_errmsg(true));
    }
    if (!args.multicast_if.empty() && toolkit::SockUtil::setMultiIF(fd, args.multicast_if.data()) != 0) {
        throw std::invalid_argument(StrPrinter << "set multicast_if failed: " << args.multicast_if << ", err: " << toolkit::get_uv_errmsg(true));
    }
}

} // namespace mediakit

#endif // ZLMEDIAKIT_RTPMULTICASTOPTIONS_H
