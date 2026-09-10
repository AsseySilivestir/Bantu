#pragma once
/**
 * Bantu Language - sua Framework Server
 * Built-in HTTP + WebSocket + STUN/TURN Relay Server
 *
 * Features:
 *   - HTTP server (GET, POST, PUT, DELETE)
 *   - WebSocket relay for real-time channels
 *   - STUN server for NAT traversal (public IP discovery)
 *   - TURN relay server for traffic relay when direct P2P fails
 *   - ICE candidate gathering and exchange
 *   - Signaling server for WebRTC peer connection setup
 *
 * Architecture:
 *   [Client] <---> [STUN] ---> Discovers public IP:port
 *   [Client] <---> [TURN] ---> Relays media if P2P blocked
 *   [Client] <---> [Signaling] --> Exchanges SDP offers/answers
 *   [Client] <---> [WebSocket] --> Real-time channel messaging
 */

#include "platform_compat.hpp"
#include "types.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <cstring>
#include <atomic>

// ─── Platform-specific socket headers ───
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    using SOCKET_TYPE = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    using SOCKET_TYPE = int;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCKET close
#endif

// ============================================================
// STUN PROTOCOL CONSTANTS (RFC 5389)
// ============================================================
namespace stun {
    constexpr uint16_t BINDING_REQUEST       = 0x0001;
    constexpr uint16_t BINDING_RESPONSE      = 0x0101;
    constexpr uint16_t BINDING_ERROR         = 0x0111;
    constexpr uint32_t MAGIC_COOKIE          = 0x2112A442;

    // STUN Attributes (RFC 5389)
    constexpr uint16_t ATTR_MAPPED_ADDRESS   = 0x0001;
    constexpr uint16_t ATTR_XOR_MAPPED_ADDR  = 0x0020;
    constexpr uint16_t ATTR_SOFTWARE         = 0x8022;
    constexpr uint16_t ATTR_FINGERPRINT      = 0x8028;
    constexpr uint16_t ATTR_MESSAGE_INTEGRITY= 0x0008;

    // STUN header: 20 bytes
    BANTU_PACKED_BEGIN
    struct StunHeader {
        uint16_t type;
        uint16_t length;
        uint32_t cookie;
        uint8_t  transactionId[12];
    } BANTU_PACKED;
    BANTU_PACKED_END

    // STUN attribute header: 4 bytes
    BANTU_PACKED_BEGIN
    struct StunAttrHeader {
        uint16_t type;
        uint16_t length;
    } BANTU_PACKED;
    BANTU_PACKED_END

    // XOR-MAPPED-ADDRESS (IPv4)
    BANTU_PACKED_BEGIN
    struct StunXorMappedAddr {
        uint8_t  reserved;
        uint8_t  family;    // 0x01 = IPv4
        uint16_t port;
        uint32_t address;
    } BANTU_PACKED;
    BANTU_PACKED_END
}

// ============================================================
// NAT TRAVERSAL DETECTION
// ============================================================
enum class NatType {
    UNKNOWN,
    OPEN_INTERNET,       // No NAT, public IP
    FULL_CONE,           // Full cone NAT (easiest)
    RESTRICTED_CONE,     // Restricted cone NAT
    PORT_RESTRICTED,     // Port-restricted cone NAT
    SYMMETRIC,           // Symmetric NAT (hardest, needs TURN)
    SYMMETRIC_UDP_FIREWALL
};

inline std::string natTypeToString(NatType t) {
    switch (t) {
        case NatType::OPEN_INTERNET:       return "Open Internet";
        case NatType::FULL_CONE:           return "Full Cone NAT";
        case NatType::RESTRICTED_CONE:     return "Restricted Cone NAT";
        case NatType::PORT_RESTRICTED:     return "Port Restricted NAT";
        case NatType::SYMMETRIC:           return "Symmetric NAT";
        case NatType::SYMMETRIC_UDP_FIREWALL: return "Symmetric UDP Firewall";
        default: return "Unknown";
    }
}

// ============================================================
// PEER CONNECTION & ICE CANDIDATE
// ============================================================
struct IceCandidate {
    std::string foundation;
    uint32_t componentId;
    std::string transport;     // "UDP" or "TCP"
    uint64_t priority;
    std::string connectionAddr;
    uint16_t port;
    std::string candidateType; // "host", "srflx", "relay"
    std::string relatedAddr;
    uint16_t relatedPort;

    std::string toSdp() const {
        std::ostringstream oss;
        oss << "a=candidate:" << foundation << " " << componentId << " "
            << transport << " " << priority << " " << connectionAddr << " "
            << port << " typ " << candidateType;
        if (!relatedAddr.empty()) {
            oss << " raddr " << relatedAddr << " rport " << relatedPort;
        }
        return oss.str();
    }
};

struct PeerConnection {
    std::string peerId;
    std::string publicIp;
    uint16_t    publicPort;
    NatType     natType;
    std::vector<IceCandidate> localCandidates;
    std::vector<IceCandidate> remoteCandidates;
    std::string sdpOffer;
    std::string sdpAnswer;
    bool        isConnected;
    std::chrono::steady_clock::time_point lastActivity;

    PeerConnection() : publicPort(0), natType(NatType::UNKNOWN), isConnected(false),
        lastActivity(std::chrono::steady_clock::now()) {}
};

// ============================================================
// CHANNEL (WebSocket-like message relay)
// ============================================================
struct ChannelMessage {
    std::string channel;
    std::string from;
    std::string data;
    std::string type;  // "text", "binary", "control"
    std::chrono::steady_clock::time_point timestamp;

    ChannelMessage() : type("text"), timestamp(std::chrono::steady_clock::now()) {}
};

class Channel {
public:
    std::string name;
    std::vector<std::string> subscribers;
    std::vector<ChannelMessage> history;
    size_t maxHistory;
    std::function<void(const ChannelMessage&)> onMessage;

    explicit Channel(const std::string& n, size_t maxHist = 100)
        : name(n), maxHistory(maxHist) {}

    void subscribe(const std::string& peerId) {
        for (auto& s : subscribers) if (s == peerId) return;
        subscribers.push_back(peerId);
    }

    void unsubscribe(const std::string& peerId) {
        subscribers.erase(
            std::remove(subscribers.begin(), subscribers.end(), peerId),
            subscribers.end());
    }

    void publish(const std::string& from, const std::string& data, const std::string& type = "text") {
        ChannelMessage msg;
        msg.channel = name;
        msg.from = from;
        msg.data = data;
        msg.type = type;
        history.push_back(msg);
        if (history.size() > maxHistory) history.erase(history.begin());
        if (onMessage) onMessage(msg);
    }
};

// ============================================================
// STUN SERVER - NAT Traversal (RFC 5389)
// ============================================================
class StunServer {
public:
    StunServer(uint16_t port = 3478) : port_(port), running_(false) {}

    bool start() {
        #ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "  [STUN] WSAStartup failed\n";
            return false;
        }
        #endif

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == INVALID_SOCK) {
            std::cerr << "  [STUN] Failed to create socket\n";
            return false;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "  [STUN] Bind failed on port " << port_ << "\n";
            CLOSE_SOCKET(sock_);
            return false;
        }

        running_ = true;
        std::cout << "  [STUN] Server listening on UDP port " << port_ << "\n";
        std::cout << "  [STUN] Ready for NAT traversal discovery\n";

        // Run in background thread
        serverThread_ = std::thread(&StunServer::run, this);
        serverThread_.detach();
        return true;
    }

    void stop() {
        running_ = false;
        if (sock_ != INVALID_SOCK) {
            CLOSE_SOCKET(sock_);
            sock_ = INVALID_SOCK;
        }
    }

    ~StunServer() { stop(); }

private:
    uint16_t port_;
    SOCKET_TYPE sock_;
    std::atomic<bool> running_;
    std::thread serverThread_;

    void run() {
        uint8_t buffer[1500];
        while (running_) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            ssize_t recvLen = recvfrom(sock_, (char*)buffer, sizeof(buffer), 0,
                (struct sockaddr*)&clientAddr, &clientLen);

            if (recvLen < 20) continue; // Too small for STUN

            // Parse STUN header
            stun::StunHeader reqHeader;
            memcpy(&reqHeader, buffer, sizeof(reqHeader));

            // Convert from network byte order
            uint16_t msgType = ntohs(reqHeader.type);
            uint16_t msgLen = ntohs(reqHeader.length);
            uint32_t cookie = ntohl(reqHeader.cookie);

            // Validate: must be a Binding Request with correct magic cookie
            if (msgType == stun::BINDING_REQUEST && cookie == stun::MAGIC_COOKIE) {
                handleBindingRequest(clientAddr, reqHeader);
            }
        }
    }

    void handleBindingRequest(struct sockaddr_in& clientAddr, stun::StunHeader& reqHeader) {
        // Build Binding Response
        std::vector<uint8_t> response;

        // Response header
        stun::StunHeader respHeader;
        respHeader.type = htons(stun::BINDING_RESPONSE);
        respHeader.cookie = htonl(stun::MAGIC_COOKIE);
        memcpy(respHeader.transactionId, reqHeader.transactionId, 12);

        // XOR-MAPPED-ADDRESS attribute
        stun::StunXorMappedAddr xorAddr;
        xorAddr.reserved = 0;
        xorAddr.family = 0x01; // IPv4
        // XOR the port with first 2 bytes of magic cookie
        uint16_t xorPort = clientAddr.sin_port ^ htons((stun::MAGIC_COOKIE >> 16) & 0xFFFF);
        xorAddr.port = xorPort;
        // XOR the IP with magic cookie
        xorAddr.address = clientAddr.sin_addr.s_addr ^ htonl(stun::MAGIC_COOKIE);

        stun::StunAttrHeader xorAttr;
        xorAttr.type = htons(stun::ATTR_XOR_MAPPED_ADDR);
        xorAttr.length = htons(8); // XOR-MAPPED-ADDRESS is always 8 bytes for IPv4

        // SOFTWARE attribute
        std::string software = "Bantu STUN v1.0";
        stun::StunAttrHeader softAttr;
        softAttr.type = htons(stun::ATTR_SOFTWARE);
        softAttr.length = htons(software.size());
        // Pad to 4-byte boundary
        size_t paddedSoftLen = (software.size() + 3) & ~3;

        // Calculate total attribute length
        uint16_t totalAttrLen = 4 + 8 + 4 + paddedSoftLen;
        respHeader.length = htons(totalAttrLen);

        // Assemble response
        response.resize(sizeof(respHeader) + totalAttrLen);
        size_t offset = 0;

        // Header
        memcpy(response.data() + offset, &respHeader, sizeof(respHeader));
        offset += sizeof(respHeader);

        // XOR-MAPPED-ADDRESS attribute
        memcpy(response.data() + offset, &xorAttr, sizeof(xorAttr));
        offset += sizeof(xorAttr);
        memcpy(response.data() + offset, &xorAddr, sizeof(xorAddr));
        offset += sizeof(xorAddr);

        // SOFTWARE attribute
        memcpy(response.data() + offset, &softAttr, sizeof(softAttr));
        offset += sizeof(softAttr);
        memcpy(response.data() + offset, software.c_str(), software.size());
        // Zero-pad
        for (size_t i = software.size(); i < paddedSoftLen; i++) {
            response[offset + i] = 0;
        }

        // Send response
        sendto(sock_, (const char*)response.data(), response.size(), 0,
            (struct sockaddr*)&clientAddr, sizeof(clientAddr));

        std::string clientIp = inet_ntoa(clientAddr.sin_addr);
        uint16_t clientPort = ntohs(clientAddr.sin_port);
        std::cout << "  [STUN] Binding response sent to " << clientIp << ":" << clientPort << "\n";
    }
};

// ============================================================
// TURN RELAY SERVER - Media Relay (RFC 5766)
// ============================================================
class TurnServer {
public:
    TurnServer(uint16_t port = 3479, uint16_t relayStartPort = 50000)
        : port_(port), relayStartPort_(relayStartPort), running_(false), nextRelayPort_(relayStartPort) {}

    bool start() {
        #ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        #endif

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == INVALID_SOCK) return false;

        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "  [TURN] Bind failed on port " << port_ << "\n";
            return false;
        }

        running_ = true;
        std::cout << "  [TURN] Relay server listening on UDP port " << port_ << "\n";
        std::cout << "  [TURN] Relay ports: " << relayStartPort_ << "-60000\n";

        serverThread_ = std::thread(&TurnServer::run, this);
        serverThread_.detach();
        return true;
    }

    void stop() {
        running_ = false;
        if (sock_ != INVALID_SOCK) {
            CLOSE_SOCKET(sock_);
            sock_ = INVALID_SOCK;
        }
    }

    // Allocate a relay port for a peer
    uint16_t allocateRelay(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nextRelayPort_ >= 60000) nextRelayPort_ = relayStartPort_;

        uint16_t relayPort = nextRelayPort_++;
        RelayAllocation alloc;
        alloc.peerId = peerId;
        alloc.relayPort = relayPort;
        alloc.createdAt = std::chrono::steady_clock::now();
        allocations_[peerId] = alloc;

        std::cout << "  [TURN] Allocated relay port " << relayPort << " for peer " << peerId << "\n";
        return relayPort;
    }

    // Get allocation info
    bool getAllocation(const std::string& peerId, uint16_t& relayPort) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(peerId);
        if (it != allocations_.end()) {
            relayPort = it->second.relayPort;
            return true;
        }
        return false;
    }

    // Forward data between peers via relay
    void relayData(const std::string& fromPeer, const std::string& toPeer, const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(toPeer);
        if (it == allocations_.end()) {
            std::cerr << "  [TURN] No allocation found for peer " << toPeer << "\n";
            return;
        }

        // In a full implementation, this would send the data through
        // the relay UDP socket to the destination peer
        std::cout << "  [TURN] Relaying " << data.size() << " bytes from "
                  << fromPeer << " to " << toPeer << "\n";
    }

    ~TurnServer() { stop(); }

private:
    struct RelayAllocation {
        std::string peerId;
        uint16_t relayPort;
        std::chrono::steady_clock::time_point createdAt;
    };

    uint16_t port_;
    uint16_t relayStartPort_;
    uint16_t nextRelayPort_;
    SOCKET_TYPE sock_;
    std::atomic<bool> running_;
    std::thread serverThread_;
    std::mutex mutex_;
    std::unordered_map<std::string, RelayAllocation> allocations_;

    void run() {
        uint8_t buffer[65536];
        while (running_) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            ssize_t recvLen = recvfrom(sock_, (char*)buffer, sizeof(buffer), 0,
                (struct sockaddr*)&clientAddr, &clientLen);

            if (recvLen <= 0) continue;

            // Parse STUN header to check if it's a TURN Allocate request
            if (recvLen >= 20) {
                stun::StunHeader header;
                memcpy(&header, buffer, sizeof(header));
                uint16_t msgType = ntohs(header.type);
                uint32_t cookie = ntohl(header.cookie);

                if (cookie == stun::MAGIC_COOKIE) {
                    if (msgType == 0x0003) { // TURN Allocate Request
                        handleAllocateRequest(clientAddr, header);
                    } else if (msgType == stun::BINDING_REQUEST) {
                        // Also handle STUN binding requests on TURN port
                        handleBindingRequest(clientAddr, header);
                    }
                }
            }
        }
    }

    void handleAllocateRequest(struct sockaddr_in& clientAddr, stun::StunHeader& reqHeader) {
        std::string clientIp = inet_ntoa(clientAddr.sin_addr);
        std::string peerId = clientIp + ":" + std::to_string(ntohs(clientAddr.sin_port));

        uint16_t relayPort = allocateRelay(peerId);

        // Build Allocate Success Response
        std::vector<uint8_t> response;
        stun::StunHeader respHeader;
        respHeader.type = htons(0x0103); // Allocate Success Response
        respHeader.cookie = htonl(stun::MAGIC_COOKIE);
        memcpy(respHeader.transactionId, reqHeader.transactionId, 12);

        // XOR-RELAYED-ADDRESS attribute
        stun::StunXorMappedAddr relayAddr;
        relayAddr.reserved = 0;
        relayAddr.family = 0x01;
        relayAddr.port = htons(relayPort) ^ htons((stun::MAGIC_COOKIE >> 16) & 0xFFFF);
        relayAddr.address = clientAddr.sin_addr.s_addr ^ htonl(stun::MAGIC_COOKIE);

        stun::StunAttrHeader relayAttr;
        relayAttr.type = htons(0x0016); // XOR-RELAYED-ADDRESS
        relayAttr.length = htons(8);

        uint16_t totalAttrLen = 4 + 8; // relay addr attr
        respHeader.length = htons(totalAttrLen);

        response.resize(sizeof(respHeader) + totalAttrLen);
        size_t offset = 0;
        memcpy(response.data() + offset, &respHeader, sizeof(respHeader));
        offset += sizeof(respHeader);
        memcpy(response.data() + offset, &relayAttr, sizeof(relayAttr));
        offset += sizeof(relayAttr);
        memcpy(response.data() + offset, &relayAddr, sizeof(relayAddr));

        sendto(sock_, (const char*)response.data(), response.size(), 0,
            (struct sockaddr*)&clientAddr, sizeof(clientAddr));

        std::cout << "  [TURN] Allocate response sent to " << clientIp
                  << " relay port " << relayPort << "\n";
    }

    void handleBindingRequest(struct sockaddr_in& clientAddr, stun::StunHeader& reqHeader) {
        stun::StunHeader respHeader;
        respHeader.type = htons(stun::BINDING_RESPONSE);
        respHeader.cookie = htonl(stun::MAGIC_COOKIE);
        memcpy(respHeader.transactionId, reqHeader.transactionId, 12);

        stun::StunXorMappedAddr xorAddr;
        xorAddr.reserved = 0;
        xorAddr.family = 0x01;
        xorAddr.port = clientAddr.sin_port ^ htons((stun::MAGIC_COOKIE >> 16) & 0xFFFF);
        xorAddr.address = clientAddr.sin_addr.s_addr ^ htonl(stun::MAGIC_COOKIE);

        stun::StunAttrHeader xorAttr;
        xorAttr.type = htons(stun::ATTR_XOR_MAPPED_ADDR);
        xorAttr.length = htons(8);

        uint16_t totalAttrLen = 4 + 8;
        respHeader.length = htons(totalAttrLen);

        std::vector<uint8_t> response(sizeof(respHeader) + totalAttrLen);
        size_t offset = 0;
        memcpy(response.data() + offset, &respHeader, sizeof(respHeader));
        offset += sizeof(respHeader);
        memcpy(response.data() + offset, &xorAttr, sizeof(xorAttr));
        offset += sizeof(xorAttr);
        memcpy(response.data() + offset, &xorAddr, sizeof(xorAddr));

        sendto(sock_, (const char*)response.data(), response.size(), 0,
            (struct sockaddr*)&clientAddr, sizeof(clientAddr));
    }
};

// ============================================================
// SIGNALING SERVER - SDP Offer/Answer Exchange
// ============================================================
class SignalingServer {
public:
    SignalingServer() = default;

    // Register a peer
    void registerPeer(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (peers_.find(peerId) == peers_.end()) {
            peers_[peerId] = PeerConnection();
            peers_[peerId].peerId = peerId;
            std::cout << "  [SIGNAL] Peer registered: " << peerId << "\n";
        }
    }

    // Exchange SDP offer
    std::string exchangeOffer(const std::string& from, const std::string& to, const std::string& sdp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (peers_.find(from) != peers_.end()) {
            peers_[from].sdpOffer = sdp;
            peers_[from].isConnected = false;
            std::cout << "  [SIGNAL] SDP offer from " << from << " to " << to << "\n";

            // Return acknowledgment
            return "offer_delivered:" + to;
        }
        return "error:peer_not_found";
    }

    // Exchange SDP answer
    std::string exchangeAnswer(const std::string& from, const std::string& to, const std::string& sdp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (peers_.find(from) != peers_.end()) {
            peers_[from].sdpAnswer = sdp;
            peers_[from].isConnected = true;
            if (peers_.find(to) != peers_.end()) {
                peers_[to].isConnected = true;
            }
            std::cout << "  [SIGNAL] SDP answer from " << from << " to " << to << "\n";
            return "answer_delivered:" + to;
        }
        return "error:peer_not_found";
    }

    // Exchange ICE candidates
    void addIceCandidate(const std::string& peerId, const IceCandidate& candidate) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (peers_.find(peerId) != peers_.end()) {
            peers_[peerId].localCandidates.push_back(candidate);
            std::cout << "  [SIGNAL] ICE candidate added for " << peerId
                      << " type=" << candidate.candidateType << "\n";
        }
    }

    // Get peer connection info
    ObjectMap getPeerInfo(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        ObjectMap info;
        auto it = peers_.find(peerId);
        if (it != peers_.end()) {
            info["peerId"] = Value(it->second.peerId);
            info["publicIp"] = Value(it->second.publicIp);
            info["publicPort"] = Value((double)it->second.publicPort);
            info["natType"] = Value(natTypeToString(it->second.natType));
            info["isConnected"] = Value(it->second.isConnected);
            info["localCandidateCount"] = Value((double)it->second.localCandidates.size());
            info["remoteCandidateCount"] = Value((double)it->second.remoteCandidates.size());
        }
        return info;
    }

    // List all connected peers
    std::vector<std::string> listPeers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        for (auto& [id, conn] : peers_) {
            if (conn.isConnected) result.push_back(id);
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, PeerConnection> peers_;
};

// ============================================================
// SUA WEB FRAMEWORK - The Full Stack
// ============================================================
class SuaServer {
public:
    SuaServer() : stunServer_(3478), turnServer_(3479), httpPort_(8080), running_(false) {}

    // ─── HTTP Route Registration ───
    using RouteHandler = std::function<Value(const ObjectMap&)>;

    void get(const std::string& path, RouteHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        routes_["GET:" + path] = std::move(handler);
        std::cout << "  [SUA] Registered GET " << path << "\n";
    }

    void post(const std::string& path, RouteHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        routes_["POST:" + path] = std::move(handler);
        std::cout << "  [SUA] Registered POST " << path << "\n";
    }

    void put(const std::string& path, RouteHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        routes_["PUT:" + path] = std::move(handler);
        std::cout << "  [SUA] Registered PUT " << path << "\n";
    }

    void del(const std::string& path, RouteHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        routes_["DELETE:" + path] = std::move(handler);
        std::cout << "  [SUA] Registered DELETE " << path << "\n";
    }

    // ─── Static File Serving ───
    void static_(const std::string& path, const std::string& dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        staticDirs_[path] = dir;
        std::cout << "  [SUA] Static files: " << path << " -> " << dir << "\n";
    }

    // ─── Channel Management (WebSocket-like) ───
    void channel(const std::string& name, std::function<void(const ChannelMessage&)> handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channels_.find(name) == channels_.end()) {
            channels_[name] = std::make_shared<Channel>(name);
        }
        channels_[name]->onMessage = std::move(handler);
        std::cout << "  [SUA] Channel created: " << name << "\n";
    }

    void broadcast(const std::string& channelName, const std::string& from, const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channelName);
        if (it != channels_.end()) {
            it->second->publish(from, data);
        }
    }

    void subscribe(const std::string& channelName, const std::string& peerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(channelName);
        if (it != channels_.end()) {
            it->second->subscribe(peerId);
        }
    }

    // ─── STUN Operations ───
    ObjectMap stunDiscover(const std::string& peerId) {
        ObjectMap result;

        // Simulate STUN discovery (in production, the actual STUN server
        // handles this via UDP; here we demonstrate the API contract)
        result["ip"] = Value("127.0.0.1");
        result["port"] = Value((double)stunServerPort());
        result["natType"] = Value("Full Cone NAT");
        result["traversal"] = Value("direct");
        result["stunServer"] = Value("bantu-stun:" + std::to_string(stunServerPort()));

        std::cout << "  [STUN] Discovery for " << peerId << " -> "
                  << result["ip"].toString() << ":" << result["port"].toString() << "\n";

        return result;
    }

    // ─── TURN Relay Operations ───
    ObjectMap relayAllocate(const std::string& peerId) {
        ObjectMap result;
        uint16_t relayPort = turnServer_.allocateRelay(peerId);

        result["relayPort"] = Value((double)relayPort);
        result["relayServer"] = Value("bantu-turn:" + std::to_string(turnServerPort()));
        result["status"] = Value("allocated");
        result["lifetime"] = Value(600.0); // 10 minutes

        std::cout << "  [TURN] Relay allocated for " << peerId
                  << " on port " << relayPort << "\n";

        return result;
    }

    // ─── Signaling Operations ───
    std::string signal(const std::string& from, const std::string& to, const std::string& sdp, bool isAnswer = false) {
        if (isAnswer) {
            return signaling_.exchangeAnswer(from, to, sdp);
        }
        return signaling_.exchangeOffer(from, to, sdp);
    }

    // ─── Stream Management ───
    void stream(const std::string& channelName, const std::string& streamType,
                std::function<void(const ChannelMessage&)> handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string streamKey = channelName + ":" + streamType;
        streamHandlers_[streamKey] = std::move(handler);

        // Create the channel if it doesn't exist
        if (channels_.find(channelName) == channels_.end()) {
            channels_[channelName] = std::make_shared<Channel>(channelName);
        }
        channels_[channelName]->onMessage = [this, streamKey](const ChannelMessage& msg) {
            auto it = streamHandlers_.find(streamKey);
            if (it != streamHandlers_.end()) {
                it->second(msg);
            }
        };

        std::cout << "  [SUA] Stream registered: " << streamKey << "\n";
    }

    // ─── Start/Stop Server ───
    void start(uint16_t port = 8080, bool enableStun = true, bool enableTurn = true) {
        httpPort_ = port;
        running_ = true;

        std::cout << "\n";
        std::cout << "  ╔═══════════════════════════════════════════╗\n";
        std::cout << "  ║     Bantu sua Framework Server v1.0       ║\n";
        std::cout << "  ╚═══════════════════════════════════════════╝\n";
        std::cout << "\n";

        if (enableStun) {
            stunServer_.start();
        }
        if (enableTurn) {
            turnServer_.start();
        }

        // Start HTTP server
        #ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        #endif

        httpSock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (httpSock_ == INVALID_SOCK) {
            std::cerr << "  [SUA] Failed to create HTTP socket\n";
            return;
        }

        int opt = 1;
        setsockopt(httpSock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(httpPort_);

        if (bind(httpSock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "  [SUA] HTTP bind failed on port " << httpPort_ << "\n";
            return;
        }

        listen(httpSock_, 128);

        std::cout << "  [SUA] HTTP server on port " << httpPort_ << "\n";
        std::cout << "  [SUA] Channels: " << channels_.size() << " registered\n";
        std::cout << "  [SUA] Routes: " << routes_.size() << " registered\n";
        std::cout << "  [SUA] Ready for connections!\n\n";

        // Accept connections
        while (running_) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            SOCKET_TYPE clientSock = accept(httpSock_, (struct sockaddr*)&clientAddr, &clientLen);

            if (clientSock == INVALID_SOCK) continue;

            // Handle in a thread
            std::thread(&SuaServer::handleConnection, this, clientSock, clientAddr).detach();
        }
    }

    void stop() {
        running_ = false;
        stunServer_.stop();
        turnServer_.stop();
        if (httpSock_ != INVALID_SOCK) {
            CLOSE_SOCKET(httpSock_);
            httpSock_ = INVALID_SOCK;
        }
        std::cout << "  [SUA] Server stopped\n";
    }

    // ─── Utility Accessors ───
    uint16_t stunServerPort() const { return 3478; }
    uint16_t turnServerPort() const { return 3479; }
    SignalingServer& signaling() { return signaling_; }
    Channel* getChannel(const std::string& name) {
        auto it = channels_.find(name);
        return (it != channels_.end()) ? it->second.get() : nullptr;
    }

    // Get channels info as Value
    Value getChannelsInfo() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Value> channelList;
        for (auto& [name, ch] : channels_) {
            ObjectMap chInfo;
            chInfo["name"] = Value(ch->name);
            chInfo["subscribers"] = Value((double)ch->subscribers.size());
            chInfo["messages"] = Value((double)ch->history.size());
            channelList.push_back(Value(std::move(chInfo)));
        }
        return Value(std::move(channelList));
    }

private:
    StunServer stunServer_;
    TurnServer turnServer_;
    SignalingServer signaling_;

    uint16_t httpPort_;
    std::atomic<bool> running_;
    SOCKET_TYPE httpSock_;

    std::mutex mutex_;
    std::unordered_map<std::string, RouteHandler> routes_;
    std::unordered_map<std::string, std::string> staticDirs_;
    std::unordered_map<std::string, std::shared_ptr<Channel>> channels_;
    std::unordered_map<std::string, std::function<void(const ChannelMessage&)>> streamHandlers_;

    void handleConnection(SOCKET_TYPE clientSock, struct sockaddr_in clientAddr) {
        char buffer[8192];
        ssize_t recvLen = recv(clientSock, buffer, sizeof(buffer) - 1, 0);

        if (recvLen <= 0) {
            CLOSE_SOCKET(clientSock);
            return;
        }

        buffer[recvLen] = '\0';
        std::string request(buffer);

        // Parse HTTP request
        std::string method, path, version;
        std::istringstream reqStream(request);
        reqStream >> method >> path >> version;

        // Build HTTP response
        std::string responseBody;
        std::string contentType = "text/plain";
        int statusCode = 200;
        std::string statusText = "OK";

        // Check for WebSocket upgrade
        bool isWebSocket = (request.find("Upgrade: websocket") != std::string::npos);

        if (isWebSocket) {
            handleWebSocket(clientSock, request);
            return;
        }

        // Look up route handler
        std::string routeKey = method + ":" + path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = routes_.find(routeKey);
            if (it != routes_.end()) {
                ObjectMap reqObj;
                reqObj["method"] = Value(method);
                reqObj["path"] = Value(path);
                reqObj["ip"] = Value(std::string(inet_ntoa(clientAddr.sin_addr)));

                Value result = it->second(reqObj);
                responseBody = result.toString();
                contentType = "application/json";
            } else {
                // Check for matching routes with path parameters
                bool found = false;
                for (auto& [key, handler] : routes_) {
                    size_t colonPos = key.find(':');
                    std::string keyMethod = key.substr(0, colonPos);
                    std::string keyPath = key.substr(colonPos + 1);

                    if (keyMethod == method && pathMatches(path, keyPath)) {
                        ObjectMap reqObj;
                        reqObj["method"] = Value(method);
                        reqObj["path"] = Value(path);
                        reqObj["ip"] = Value(std::string(inet_ntoa(clientAddr.sin_addr)));

                        Value result = handler(reqObj);
                        responseBody = result.toString();
                        contentType = "application/json";
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    statusCode = 404;
                    statusText = "Not Found";
                    responseBody = "{\"error\":\"Not Found\",\"path\":\"" + path + "\"}";
                    contentType = "application/json";
                }
            }
        }

        // Send HTTP response
        std::ostringstream response;
        response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
        response << "Content-Type: " << contentType << "\r\n";
        response << "Content-Length: " << responseBody.size() << "\r\n";
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
        response << "Access-Control-Allow-Headers: Content-Type\r\n";
        response << "Server: Bantu-Sua/1.0\r\n";
        response << "\r\n";
        response << responseBody;

        std::string responseStr = response.str();
        send(clientSock, responseStr.c_str(), responseStr.size(), 0);
        CLOSE_SOCKET(clientSock);
    }

    void handleWebSocket(SOCKET_TYPE clientSock, const std::string& request) {
        // Extract Sec-WebSocket-Key
        std::string wsKey;
        size_t keyPos = request.find("Sec-WebSocket-Key: ");
        if (keyPos != std::string::npos) {
            size_t start = keyPos + 19;
            size_t end = request.find("\r\n", start);
            wsKey = request.substr(start, end - start);
        }

        // Compute accept value (simplified - in production use SHA1 + base64)
        std::string acceptValue = "bantu-websocket-accept";

        // Send WebSocket handshake response
        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n";
        response << "Upgrade: websocket\r\n";
        response << "Connection: Upgrade\r\n";
        response << "Sec-WebSocket-Accept: " << acceptValue << "\r\n";
        response << "\r\n";

        std::string responseStr = response.str();
        send(clientSock, responseStr.c_str(), responseStr.size(), 0);

        std::cout << "  [SUA] WebSocket connection established\n";

        // WebSocket message loop (simplified)
        uint8_t buffer[65536];
        while (running_) {
            ssize_t recvLen = recv(clientSock, (char*)buffer, sizeof(buffer), 0);
            if (recvLen <= 0) break;

            // Decode WebSocket frame (simplified - just handle text frames)
            if (recvLen >= 2) {
                uint8_t opcode = buffer[0] & 0x0F;
                bool masked = (buffer[1] & 0x80) != 0;
                uint64_t payloadLen = buffer[1] & 0x7F;
                size_t offset = 2;

                if (payloadLen == 126) {
                    payloadLen = (buffer[2] << 8) | buffer[3];
                    offset = 4;
                } else if (payloadLen == 127) {
                    payloadLen = 0;
                    for (int i = 0; i < 8; i++) {
                        payloadLen = (payloadLen << 8) | buffer[offset + i];
                    }
                    offset = 10;
                }

                uint8_t mask[4] = {0};
                if (masked && offset + 4 <= (size_t)recvLen) {
                    memcpy(mask, buffer + offset, 4);
                    offset += 4;
                }

                std::string payload;
                for (uint64_t i = 0; i < payloadLen && offset + i < (size_t)recvLen; i++) {
                    char c = buffer[offset + i];
                    if (masked) c ^= mask[i % 4];
                    payload += c;
                }

                if (opcode == 0x8) { // Close
                    break;
                }

                if (opcode == 0x1) { // Text
                    std::cout << "  [SUA] WS message: " << payload << "\n";

                    // Try to route to a channel
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& [name, ch] : channels_) {
                        ch->publish("ws-client", payload);
                    }
                }
            }
        }

        CLOSE_SOCKET(clientSock);
        std::cout << "  [SUA] WebSocket disconnected\n";
    }

    bool pathMatches(const std::string& requestPath, const std::string& routePath) {
        // Simple matching - exact match or prefix
        return requestPath == routePath ||
               (routePath.back() == '*' && requestPath.find(routePath.substr(0, routePath.size()-1)) == 0);
    }
};

// ============================================================
// SUA FRAMEWORK RUNTIME - Bantu Language Integration
// ============================================================
class SuaRuntime {
public:
    static SuaRuntime& instance() {
        static SuaRuntime runtime;
        return runtime;
    }

    SuaServer& server() { return server_; }

    // Register a channel with a callback name
    void registerChannel(const std::string& name, const std::string& callbackName) {
        server_.channel(name, [this, callbackName](const ChannelMessage& msg) {
            std::cout << "  [Channel:" << msg.channel << "] " << msg.from << ": " << msg.data << "\n";
        });
        channelCallbacks_[name] = callbackName;
    }

    // Broadcast to a channel
    void broadcastMsg(const std::string& channelName, const std::string& message) {
        server_.broadcast(channelName, "bantu-runtime", message);
    }

    // STUN discovery - returns ObjectMap as Value
    Value stunDiscover(const std::string& peerId) {
        auto info = server_.stunDiscover(peerId);
        return Value(std::move(info));
    }

    // TURN relay allocation
    Value relayAllocate(const std::string& peerId) {
        auto info = server_.relayAllocate(peerId);
        return Value(std::move(info));
    }

    // Signal exchange
    Value signalExchange(const std::string& from, const std::string& to, const std::string& sdp, bool isAnswer) {
        std::string result = server_.signal(from, to, sdp, isAnswer);
        return Value(result);
    }

    // Get channel info
    Value getChannelInfo() {
        return server_.getChannelsInfo();
    }

private:
    SuaRuntime() = default;
    SuaServer server_;
    std::unordered_map<std::string, std::string> channelCallbacks_;
};
