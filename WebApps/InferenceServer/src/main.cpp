#include <string>
#include <iostream>
#include <thread>
#include <csignal>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include "InferenceServer.h"
#include "../../../HttpServer/include/utils/ConfigLoader.h"
#include "../../../HttpServer/include/utils/LogUtil.h"

int main(int argc, char* argv[])
{
  LOG_INFO << "pid = " << getpid();

  std::string configPath = "config.json";

  // Parse CLI args
  int opt;
  const char* str = "p:t:l:c:";
  while ((opt = getopt(argc, argv, str)) != -1)
  {
    switch (opt)
    {
      case 'c':
        configPath = optarg;
        break;
      default:
        break;
    }
  }

  // Load config
  AppConfig cfg = loadConfig(configPath);

  // Re-parse for overrides (port, threads, log_level)
  optind = 1;
  while ((opt = getopt(argc, argv, str)) != -1)
  {
    switch (opt)
    {
      case 'p':
        cfg.server.port = atoi(optarg);
        break;
      case 't':
        cfg.server.threads = atoi(optarg);
        break;
      case 'l':
        cfg.server.log_level = optarg;
        break;
      default:
        break;
    }
  }

  // Init spdlog
  initSpdLog(cfg.logging.file, cfg.logging.level);
  initAccessLog(cfg.logging.access_log);

  // Apply muduo log level
  if (cfg.server.log_level == "TRACE")
    muduo::Logger::setLogLevel(muduo::Logger::TRACE);
  else if (cfg.server.log_level == "DEBUG")
    muduo::Logger::setLogLevel(muduo::Logger::DEBUG);
  else if (cfg.server.log_level == "INFO")
    muduo::Logger::setLogLevel(muduo::Logger::INFO);
  else
    muduo::Logger::setLogLevel(muduo::Logger::WARN);

  InferenceServer server(cfg);
  server.setConfigPath(configPath);
  server.setThreadNum(cfg.server.threads);

  // Block signals in this thread; they will be handled by sigwait thread
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGINT);
  sigaddset(&sigset, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

  std::thread sigThread([&server, timeoutMs = cfg.server.shutdown_timeout_ms]() {
    sigset_t waitSet;
    sigemptyset(&waitSet);
    sigaddset(&waitSet, SIGINT);
    sigaddset(&waitSet, SIGTERM);

    int sig = 0;
    sigwait(&waitSet, &sig);
    LOG_INFO << "Received signal " << sig << ", starting graceful shutdown";
    server.gracefulShutdown(std::chrono::milliseconds(timeoutMs));
  });

  server.start();
  sigThread.join();
  return 0;
}
