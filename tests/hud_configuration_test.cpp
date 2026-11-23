// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/hud_configuration.hpp>

#include <iostream>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void rejects(const std::string& text) {
  try {
    (void)headunit::parse_hud_configuration(text);
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("accepted unsafe HUD configuration");
}
}  // namespace
int main() {
  try {
    const auto config = headunit::parse_hud_configuration("output=DSI-1\n");
    require(config.calibration.reflect_horizontal && config.fps == 20 && config.width == 960 &&
                config.main_output == "HDMI-A-1" && config.country.empty(),
            "incorrect defaults");
    require(headunit::parse_hud_configuration("output=DSI-1\ncountry=GB\n").country == "GB",
            "lost country fallback");
    require(headunit::parse_hud_configuration("output=DSI-1\ncountry=auto\n").country.empty(),
            "auto country not unknown");
    rejects("output=DSI-1\ncountry=gb\n");
    rejects("output=DSI-1\ncountry=GBR\n");
    const auto custom = headunit::parse_hud_configuration(
        "output=HUD-1\nmain_output=MAIN-1\ncluster_output=CLUSTER-1\n"
        "reflect_horizontal=false\nreflect_vertical=true\nquarter_turns=2\n"
        "scale=0.8\noffset_x=0.1\noffset_y=-0.2\nwidth=1280\nheight=480\nfps=30\n"
        "top_left_x=0.1\ntop_left_y=0.1\ntop_right_x=0.9\ntop_right_y=0.05\n"
        "bottom_right_x=0.95\nbottom_right_y=0.9\nbottom_left_x=0.05\nbottom_left_y=0.95\n");
    require(custom.calibration.corners && custom.calibration.reflect_vertical &&
                custom.calibration.quarter_turns == 2 && custom.width == 1280,
            "lost calibration");
    for (const auto& bad : {"output=HDMI-A-1\n", "output=HDMI-A-2\n",
                            "output=DSI-1\nmain_output=HDMI-A-2\n", "output=DSI-1\noutput=DSI-2\n",
                            "output=DSI-1\nfps=60\n", "output=DSI-1\nreflect_horizontal=1\n",
                            "output=DSI-1\nscale=nan\n", "output=DSI-1\nscale=1.01\n",
                            "output=DSI-1\nquarter_turns=4\n", "output=DSI-1\noffset_x=1\n",
                            "output=DSI-1\nwidth=8192\n", "output=DSI-1\ntop_left_x=0.1\n",
                            "output=DSI-1\nunknown=true\n", "output=../hud\n", "# no output\n"})
      rejects(bad);
    rejects(std::string(4097, 'x'));
    rejects(std::string("output=DSI-1\n\0", 14));
    rejects(
        "output=DSI-1\ntop_left_x=0\ntop_left_y=0\ntop_right_x=0\ntop_right_y=1\n"
        "bottom_right_x=1\nbottom_right_y=0\nbottom_left_x=1\nbottom_left_y=1\n");
    const std::string valid = "output=DSI-1\n";
    {
      char directory[] = "/tmp/headunit-hud-config-XXXXXX";
      require(mkdtemp(directory), "temporary HUD configuration directory failed");
      const std::filesystem::path root(directory), file = root / "hud.conf";
      struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
      } cleanup{root};
      const auto write = [&](const std::filesystem::path& path, const std::string& text) {
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        stream.close();
        require(bool(stream), "HUD configuration publication failed");
      };
      write(file, valid);
      require(headunit::load_hud_configuration(file).output_name == "DSI-1",
              "bounded HUD configuration file was not admitted");
      write(root / "replacement", valid);
      std::filesystem::rename(root / "replacement", file);
      require(headunit::load_hud_configuration(file).output_name == "DSI-1",
              "complete atomic HUD configuration replacement was not admitted");
      const auto link = root / "link", oversized = root / "oversized", fifo = root / "fifo";
      std::filesystem::create_symlink(file, link);
      require(headunit::load_hud_configuration(link).output_name == "DSI-1",
              "explicit HUD configuration source link was not admitted");
      write(oversized, valid + std::string(4096, ' '));
      require(mkfifo(fifo.c_str(), 0600) == 0, "HUD configuration pipe fixture failed");
      for (const auto& path : {root, oversized}) {
        bool rejected = false;
        try {
          (void)headunit::load_hud_configuration(path);
        } catch (const std::exception&) {
          rejected = true;
        }
        require(rejected, "nonregular or oversized HUD configuration admitted");
      }
      // Bound the regression itself: an old blocking loader must fail this test
      // instead of hanging CI waiting for a nonexistent pipe writer.
      const auto child = fork();
      require(child >= 0, "HUD configuration reader fixture could not fork");
      if (!child) {
        try {
          (void)headunit::load_hud_configuration(fifo);
        } catch (const std::exception&) {
          _exit(0);
        }
        _exit(1);
      }
      int status = 0;
      pid_t completed = 0;
      for (unsigned attempt = 0; attempt < 400 && !completed; ++attempt) {
        completed = waitpid(child, &status, WNOHANG);
        if (!completed) usleep(5000);
      }
      if (!completed) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
      }
      require(completed == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "HUD configuration pipe blocked startup or was admitted");
    }
    std::cout << "HUD configuration checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
