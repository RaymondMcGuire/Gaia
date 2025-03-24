/*
 * File: Parser.h
 * Module: Parser
 * Created Date: 2025-03-24
 * Author: Xu WANG
 * -----
 * Last Modified: 2025-03-24
 * Modified By: Xu WANG
 * -----
 * Copyright (c) 2025 Xu WANG
 */

#pragma once

#include "../VersionTracker/VersionTracker.h"
#include <MeshFrame/Utility/cxxopts.hpp>

namespace GAIA {
struct CommandParser {
  bool gui = true;
  bool showGitInfo = false;
  bool runOnCPU = false;
  std::string recoveryStateFile = "";
  std::string repoRoot = "";

  CommandParser() : options("EBDAppParams", "GAIA physics application.") {
    options.add_options()("g,gui", "", cxxopts::value<bool>(gui))(
        "r,recoveryState", "", cxxopts::value<std::string>(recoveryStateFile))(
        "R,repoRoot", "", cxxopts::value<std::string>(repoRoot))(
        "CPU", "", cxxopts::value<bool>(runOnCPU))(
        "git", "", cxxopts::value<bool>(showGitInfo));
  }

  void parse(int argc, char **argv) {
    try {
      auto result = options.parse(argc, argv);
    } catch (const cxxopts::OptionException &e) {
      std::cout << "error parsing options: " << e.what() << std::endl;
      std::cout << options.help();
      exit(1);
    }
  }

  cxxopts::Options options;
};
} // namespace GAIA
