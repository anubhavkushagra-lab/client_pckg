/*
 * load_client.cpp  -  Synchronous benchmark client
 *
 * Usage:  ./load_client <THREADS> <SECONDS> [SERVER_IP] [PAYLOAD_BYTES]
 */
#include "kv.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Helper to read certificate files
std::string ReadFile(const std::string& path) {
    std::ifstream t(path);
    if (!t.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(t)),
                        std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Usage: ./load_client <THREADS> <SECONDS> [SERVER_IP] "
                 "[PAYLOAD_BYTES] [TARGET_NAME_OVERRIDE]\n"
              << "  e.g. ./load_client 400 60 192.168.0.109 64 kv-server\n";
    return 0;
  }

  int threads = std::stoi(argv[1]);
  int target_seconds = std::stoi(argv[2]);
  std::string server_ip = (argc >= 4) ? argv[3] : "127.0.0.1";
  int payload_bytes = (argc >= 5) ? std::stoi(argv[4]) : 64;
  std::string target_name_ov = (argc >= 6) ? argv[5] : "kv-server";

    // SSL/TLS Setup
    std::string ca_cert = ReadFile("ca.crt");
    
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ca_cert;
    auto channel_creds = ca_cert.empty() ? grpc::InsecureChannelCredentials() 
                                         : grpc::SslCredentials(ssl_opts);

    grpc::ChannelArguments login_args;
    if (!ca_cert.empty()) {
        login_args.SetSslTargetNameOverride(target_name_ov);
    }

    // Login once
    auto plain_ch   = grpc::CreateCustomChannel(
                          "ipv4:" + server_ip + ":50063",
                          channel_creds, login_args);
    auto login_stub = kv::KVService::NewStub(plain_ch);

  std::string jwt;
  {
    grpc::ClientContext ctx;
    kv::LoginRequest    req;
    kv::LoginResponse   resp;
    req.set_api_key("initial-pass");
    req.set_client_id("bench");
    auto st = login_stub->Login(&ctx, req, &resp);
    if (!st.ok() || resp.jwt_token().empty()) {
      std::cerr << "Login failed: " << st.error_message() << "\n";
      return 1;
    }
    jwt = resp.jwt_token();
    std::cout << "Login OK. Starting benchmark...\n";
  }

  // Stub pool
  const int FIRST_PORT = 50063, LAST_PORT = 50070;
  const int CHANNELS_PER_PORT = 8;

  std::vector<std::unique_ptr<kv::KVService::Stub>> stubs;
  for (int p = FIRST_PORT; p <= LAST_PORT; ++p) {
    for (int c = 0; c < CHANNELS_PER_PORT; ++c) {
      grpc::ChannelArguments args;
      args.SetMaxReceiveMessageSize(-1);
      args.SetMaxSendMessageSize(-1);
      args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
      if (!ca_cert.empty()) {
          args.SetSslTargetNameOverride(target_name_ov);
      }
      auto ch = grpc::CreateCustomChannel(
          "ipv4:" + server_ip + ":" + std::to_string(p),
          channel_creds, args);
      stubs.push_back(kv::KVService::NewStub(ch));
    }
  }

  // Pre-generate keys
  const long KEY_WINDOW = 100000L;
  std::vector<std::string> all_keys((size_t)threads * KEY_WINDOW);
  for (int t = 0; t < threads; ++t) {
    long base = (long)t * KEY_WINDOW;
    for (long k = 0; k < KEY_WINDOW; ++k) {
      char buf[32];
      int len = std::snprintf(buf, sizeof(buf), "k%d_%ld", t, k);
      all_keys[base + k].assign(buf, len);
    }
  }

  std::string value(payload_bytes, 'x');

    struct Stats {
        long ok = 0;
        long fail = 0;
        long total_lat_ns = 0;
        long min_lat_ns = 2000000000000L;
        long max_lat_ns = 0;
    };
    std::vector<Stats> thread_stats(threads);

    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t]() {
      auto& s = thread_stats[t];
      long j = 0;

      auto *stub = stubs[t % stubs.size()].get();
      long base = (long)t * KEY_WINDOW;

      while (true) {
        if ((j & 15) == 0) {
          auto now = std::chrono::steady_clock::now();
          if (std::chrono::duration_cast<std::chrono::seconds>(now - start)
                  .count() >= target_seconds)
            break;
        }

        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", jwt);

        kv::SingleRequest req;
        kv::SingleResponse resp;
        req.set_type(kv::PUT);
        req.set_key(all_keys[base + (j % KEY_WINDOW)]);
        req.set_value(value);

        auto req_start = std::chrono::high_resolution_clock::now();
        auto st = stub->ExecuteSingle(&ctx, req, &resp);
        auto req_end   = std::chrono::high_resolution_clock::now();

        if (st.ok() && resp.success()) {
          long lat = std::chrono::duration_cast<std::chrono::nanoseconds>(req_end - req_start).count();
          s.total_lat_ns += lat;
          if (lat < s.min_lat_ns) s.min_lat_ns = lat;
          if (lat > s.max_lat_ns) s.max_lat_ns = lat;
          ++s.ok;
        } else {
          ++s.fail;
        }
        ++j;
      }
    });
  }

  for (auto &th : pool)
    th.join();

  auto end = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(end - start).count();
  
  long total_ok = 0;
  long total_lat_ns = 0;
  long min_lat_ns = 2000000000000L;
  long max_lat_ns = 0;

  for (const auto& s : thread_stats) {
      total_ok += s.ok;
      total_lat_ns += s.total_lat_ns;
      if (s.min_lat_ns < min_lat_ns) min_lat_ns = s.min_lat_ns;
      if (s.max_lat_ns > max_lat_ns) max_lat_ns = s.max_lat_ns;
  }

  double rps = (total_ok > 0) ? (double)total_ok / elapsed : 0;
  double avg_lat_us = (total_ok > 0) ? (double)total_lat_ns / total_ok / 1000.0 : 0;

  std::cout << "\n--- Benchmark Results ---\n"
            << "  Server IP:    " << server_ip << "\n"
            << "  Threads:      " << threads << "\n"
            << "  Successful:   " << total_ok << "\n"
            << "  Total Time:   " << std::fixed << std::setprecision(2) << elapsed << " s\n"
            << "  Throughput:   " << (long)rps << " req/s\n"
            << "  Avg Latency:  " << (long)avg_lat_us << " us\n"
            << "  Min Latency:  " << (long)(min_lat_ns / 1000.0) << " us\n"
            << "  Max Latency:  " << (long)(max_lat_ns / 1000.0) << " us\n"
            << "-------------------------\n";
  return 0;
}
