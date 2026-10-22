// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: MIT
#include "../../native/rar_worker/worker_io.hpp"
#include <cstdlib>
int main(int argc,char** argv) {
  if(argc!=4)return 2;
  if(!worker::persist(argv[1],std::string(argv[2])+" "+argv[3],1048576))return 23;
  for(unsigned i=0;i<150000;++i)worker::say("p 123");
  worker::say("ok");
  return worker::journal_failed?3:0;
}
