// Tiny interactive shell:  lsm_cli <dir>
//   put <k> <v> | get <k> | del <k> | scan <start> [end] [limit] | flush | compact | stats | quit

#include <iostream>
#include <sstream>
#include <string>

#include "lsm/db.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: lsm_cli <db-dir>\n";
    return 2;
  }
  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(lsm::Options{}, argv[1], &db);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  std::string line;
  while (std::cout << "lsm> " << std::flush, std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd, a, b;
    in >> cmd;
    if (cmd == "put" && in >> a >> b) {
      std::cout << db->Put({}, a, b).ToString() << "\n";
    } else if (cmd == "get" && in >> a) {
      std::string v;
      s = db->Get({}, a, &v);
      std::cout << (s.ok() ? v : s.ToString()) << "\n";
    } else if (cmd == "del" && in >> a) {
      std::cout << db->Delete({}, a).ToString() << "\n";
    } else if (cmd == "scan" && in >> a) {
      size_t limit = 20;
      in >> b >> limit;
      std::vector<std::pair<std::string, std::string>> out;
      s = db->Scan({}, a, b, limit, &out);
      for (auto& [k, v] : out) std::cout << k << " = " << v << "\n";
      if (!s.ok()) std::cout << s.ToString() << "\n";
    } else if (cmd == "flush") {
      std::cout << db->Flush().ToString() << "\n";
    } else if (cmd == "compact") {
      std::cout << db->CompactAll().ToString() << "\n";
    } else if (cmd == "stats") {
      auto st = db->GetStats();
      std::cout << "memtable " << st.memtable_bytes << " B, flushes " << st.flushes
                << ", compactions " << st.compactions << "\n";
      for (int l = 0; l < 7; ++l)
        if (st.files_per_level[l])
          std::cout << "  L" << l << ": " << st.files_per_level[l] << " files, "
                    << st.bytes_per_level[l] << " B\n";
    } else if (cmd == "quit" || cmd == "exit") {
      break;
    } else if (!cmd.empty()) {
      std::cout << "commands: put get del scan flush compact stats quit\n";
    }
  }
  return 0;
}
