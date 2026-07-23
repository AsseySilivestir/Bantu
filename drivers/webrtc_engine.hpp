#pragma once
/**
*added!!!
 * Bantu v1.2.1 — WebRTC Engine (libdatachannel)
 *
 * Real WebRTC peer-to-peer connectivity for Bantu. When Bantu is
 * compiled with libdatachannel available (`-DHAS_RTC` and
 * `<rtc/rtc.hpp>` on the include path), `sua.webrtc.*` calls route
 * to a live `rtc::PeerConnection` with proper ICE / DTLS / SCTP
 * negotiation.
 *
 * Build (Linux):
 *   sudo apt install libdatachannel-dev
 *   cmake .. -DBANTU_WEBRTC=ON
 *
 * Build (Windows / vcpkg):
 *   vcpkg install datachannel
 *   cmake .. -DBANTU_WEBRTC=ON -DCMAKE_TOOLCHAIN_FILE=...
 *
 * Public API surface mirrors what `sua.webrtc.*` exposes in Bantu:
 *   peer(id)            -> Peer
 *   createOffer(peer)   -> { type: "offer",    sdp, peer }
 *   createAnswer(peer)  -> { type: "answer",   sdp, peer }
 *   addIceCandidate(peer, candidate)
 *   dataChannel(name)   -> DataChannel
 *   send(channel, msg)
 *   close(peer)
 */

#ifdef HAS_RTC
#include <rtc/rtc.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/datachannel.hpp>
#endif

#include <string>
#include <memory>
#include <unordered_map>
#include <iostream>
#include <vector>
#include <mutex>

namespace bantu {

class WebRtcEngine {
public:
#ifdef HAS_RTC
    WebRtcEngine() { rtc::InitLogger(rtc::LogLevel::Warning); }
    ~WebRtcEngine() { peers_.clear(); }

    std::string createPeer(const std::string& id) {
        auto pc = std::make_shared<rtc::PeerConnection>();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            peers_[id] = pc;
        }
        std::cout << "  [WEBRTC] libdatachannel peer created: " << id << "\n";
        return id;
    }

    std::string createOffer(const std::string& id) {
        auto pc = peer(id);
        if (!pc) return "";
        pc->setLocalDescription(rtc::Description::Type::Offer);
        // Wait briefly for SDP to be generated
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto desc = pc->localDescription();
        return std::string(desc ? desc.value() : "");
    }

    std::string createAnswer(const std::string& id) {
        auto pc = peer(id);
        if (!pc) return "";
        pc->setLocalDescription(rtc::Description::Type::Answer);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto desc = pc->localDescription();
        return std::string(desc ? desc.value() : "");
    }

    bool setRemoteDescription(const std::string& id, const std::string& sdp,
                              const std::string& type) {
        auto pc = peer(id);
        if (!pc) return false;
        rtc::Description d(sdp, type == "offer" ? rtc::Description::Type::Offer
                                                : rtc::Description::Type::Answer);
        pc->setRemoteDescription(d);
        return true;
    }

    bool addIceCandidate(const std::string& id, const std::string& candidate,
                         const std::string& mid) {
        auto pc = peer(id);
        if (!pc) return false;
        rtc::Candidate c(candidate, mid);
        pc->addRemoteCandidate(c);
        return true;
    }

    std::string openDataChannel(const std::string& peerId, const std::string& label) {
        auto pc = peer(peerId);
        if (!pc) return "";
        auto dc = pc->createDataChannel(label);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            channels_[peerId + "/" + label] = dc;
        }
        return label;
    }

    bool send(const std::string& key, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = channels_.find(key);
        if (it == channels_.end()) return false;
        it->second->send(msg);
        return true;
    }

    void close(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        peers_.erase(id);
        // also drop channels under this peer
        for (auto it = channels_.begin(); it != channels_.end(); ) {
            if (it->first.find(id + "/") == 0) it = channels_.erase(it);
            else ++it;
        }
    }

private:
    std::shared_ptr<rtc::PeerConnection> peer(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = peers_.find(id);
        return (it == peers_.end()) ? nullptr : it->second;
    }

    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<rtc::PeerConnection>> peers_;
    std::unordered_map<std::string, std::shared_ptr<rtc::DataChannel>> channels_;
#else
    // Stub mode (default build): no-op engine. The evaluator's built-in
    // simulation handles `sua.webrtc.*` calls in this case.
    std::string createPeer(const std::string&) { return ""; }
    std::string createOffer(const std::string&) { return ""; }
    std::string createAnswer(const std::string&) { return ""; }
    bool setRemoteDescription(const std::string&, const std::string&, const std::string&) { return false; }
    bool addIceCandidate(const std::string&, const std::string&, const std::string&) { return false; }
    std::string openDataChannel(const std::string&, const std::string&) { return ""; }
    bool send(const std::string&, const std::string&) { return false; }
    void close(const std::string&) {}
#endif
};

} // namespace bantu
