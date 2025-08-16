#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/systems/system.h"
#include <boost/dll/alias.hpp> // for BOOST_DLL_ALIAS
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

struct Unit_Script_System_Script {
  std::string script;
  std::string short_name;
};

class Unit_Script : public Plugin_Api {
  std::vector<Unit_Script_System_Script> system_scripts;
  std::map<long, long> unit_affiliations;

private:
  void execute_script_async(const std::string &script, const std::vector<std::string> &args) {
    pid_t pid1 = fork();
    if (pid1 == 0) {
      // First child process
      pid_t pid2 = fork();
      if (pid2 == 0) {
        // Grandchild process - executes the script
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(script.c_str()));
        for (const auto &arg : args) {
          argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(script.c_str(), argv.data());
        _exit(127); // execvp failed
      } else if (pid2 < 0) {
        BOOST_LOG_TRIVIAL(error) << "unit_script: fork: " << strerror(errno);
        _exit(1);
      }
      // First child exits immediately, orphaning grandchild to init
      _exit(0);
    } else if (pid1 > 0) {
      // Parent process - wait for first child to exit
      int status;
      waitpid(pid1, &status, 0);
    } else {
      BOOST_LOG_TRIVIAL(error) << "unit_script: fork: " << strerror(errno);
    }
  }

public:
  std::string get_system_script(std::string short_name) {
    for (std::vector<Unit_Script_System_Script>::iterator it = system_scripts.begin(); it != system_scripts.end(); ++it) {
      Unit_Script_System_Script system_script = *it;
      if (system_script.short_name == short_name) {
        return system_script.script;
      }
    }
    return "";
  }

  int unit_registration(System *sys, long source_id) {
    unit_affiliations[source_id] = 0;
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "on"};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int unit_deregistration(System *sys, long source_id) {
    unit_affiliations[source_id] = -1;
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "off"};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }
  int unit_acknowledge_response(System *sys, long source_id) {
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "ackresp"};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int unit_group_affiliation(System *sys, long source_id, long talkgroup_num) {
    unit_affiliations[source_id] = talkgroup_num;
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<unsigned long> talkgroup_patches = sys->get_talkgroup_patch(talkgroup_num);
      std::string patch_string;
      bool first = true;
      BOOST_FOREACH (auto &TGID, talkgroup_patches) {
        if (!first) {
          patch_string += ",";
        }
        first = false;
        patch_string += std::to_string(TGID);
      }
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "join", std::to_string(talkgroup_num), patch_string};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int unit_data_grant(System *sys, long source_id) {
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "data"};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int unit_answer_request(System *sys, long source_id, long talkgroup) {
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "ans_req", std::to_string(talkgroup)};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int unit_location(System *sys, long source_id, long talkgroup_num) {
    unit_affiliations[source_id] = talkgroup_num;
    std::string system_script = get_system_script(sys->get_short_name());
    if ((system_script != "") && (source_id != 0)) {
      std::vector<unsigned long> talkgroup_patches = sys->get_talkgroup_patch(talkgroup_num);
      std::string patch_string;
      bool first = true;
      BOOST_FOREACH (auto &TGID, talkgroup_patches) {
        if (!first) {
          patch_string += ",";
        }
        first = false;
        patch_string += std::to_string(TGID);
      }
      std::vector<std::string> args = {sys->get_short_name(), std::to_string(source_id), "location", std::to_string(talkgroup_num), patch_string};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int call_start(Call *call) {
    long talkgroup_num = call->get_talkgroup();
    long source_id = call->get_current_source_id();
    std::string short_name = call->get_short_name();
    std::string system_script = get_system_script(short_name);
    if ((system_script != "") && (source_id != 0)) {
      std::vector<unsigned long> talkgroup_patches = call->get_system()->get_talkgroup_patch(talkgroup_num);
      std::string patch_string;
      bool first = true;
      BOOST_FOREACH (auto &TGID, talkgroup_patches) {
        if (!first) {
          patch_string += ",";
        }
        first = false;
        patch_string += std::to_string(TGID);
      }
      std::vector<std::string> args = {short_name, std::to_string(source_id), "call", std::to_string(talkgroup_num), patch_string};
      execute_script_async(system_script, args);
      return 0;
    }
    return 1;
  }

  int parse_config(json config_data) {

    for (json element : config_data["systems"]) {
      Unit_Script_System_Script system_script;
      system_script.script = element.value("unitScript", "");
      system_script.short_name = element.value("shortName", "");
      if (system_script.script != "") {
        BOOST_LOG_TRIVIAL(info) << "\t- [" << system_script.short_name << "]: " << system_script.script;
        this->system_scripts.push_back(system_script);
      }
    }

    return 0;
  }

  static boost::shared_ptr<Unit_Script> create() {
    return boost::shared_ptr<Unit_Script>(
        new Unit_Script());
  }
};

BOOST_DLL_ALIAS(
    Unit_Script::create, // <-- this function is exported with...
    create_plugin        // <-- ...this alias name
)
