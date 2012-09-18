// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
* Ceph - scalable distributed file system
*
* Copyright (C) 2012 Inktank, Inc.
*
* This is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License version 2.1, as published by the Free Software
* Foundation. See file COPYING.
*/
#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <set>
#include <boost/scoped_ptr.hpp>

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/debug.h"
#include "common/config.h"

#include "mon/MonitorDBStore.h"
#include "mon/MonitorStore.h"

using namespace std;

class MonitorStoreConverter {

  boost::scoped_ptr<MonitorDBStore> db;
  boost::scoped_ptr<MonitorStore> store;

  map<version_t,pair<string,version_t> > gv_to_machine_version;
  version_t highest_last_pn;
  version_t highest_accepted_pn;

 public:
  MonitorStoreConverter(string &store_path, string &db_store_path)
    : db(0), store(0),
      highest_last_pn(0), highest_accepted_pn(0)
  {
    MonitorStore *store_ptr = new MonitorStore(store_path);
    assert(!store_ptr->mount());
    store.reset(store_ptr);

    MonitorDBStore *db_ptr = new MonitorDBStore(db_store_path);
    db.reset(db_ptr);
  }

  int convert() {

    int r;

    r = _convert_machines();
    assert(r == 0);

    return 0;
  }

  bool match() {
    return true;
  }

 private:

  set<string> _get_machines_names() {
    set<string> names;
    names.insert("auth");
    names.insert("logm");
    names.insert("mdsmap");
    names.insert("monmap");
    names.insert("osdmap");
    names.insert("pgmap");

    return names;
  }

  int _convert_machines(string machine) {
    std::cout << __func__ << " " << machine << std::endl;

    version_t first_committed =
      store->get_int(machine.c_str(), "first_committed");
    version_t last_committed =
      store->get_int(machine.c_str(), "last_committed");

    version_t accepted_pn = store->get_int(machine.c_str(), "accepted_pn");
    version_t last_pn = store->get_int(machine.c_str(), "last_pn");

    if (accepted_pn > highest_accepted_pn)
      highest_accepted_pn = accepted_pn;
    if (last_pn > highest_last_pn)
      highest_last_pn = last_pn;

    string machine_gv(machine);
    machine_gv.append("_gv");
    bool has_gv = true;

    if (!store->exists_bl_ss(machine_gv.c_str())) {
      std::cerr << __func__ << " " << machine
		<< " no gv dir '" << machine_gv << "'" << std::endl;
      has_gv = false;
    }

    for (version_t ver = first_committed; ver <= last_committed; ver++) {
      if (!store->exists_bl_sn(machine.c_str(), ver)) {
	std::cerr << __func__ << " " << machine
		  << " ver " << ver << " dne" << std::endl;
	continue;
      }

      bufferlist bl;
      int r = store->get_bl_sn(bl, machine.c_str(), ver);
      assert(r >= 0);
      std::cout << __func__ << " " << machine
		<< " ver " << ver << " bl " << bl.length() << std::endl;

      MonitorDBStore::Transaction tx;
      tx.put(machine, ver, bl);
      tx.put(machine, "last_committed", ver);

      if (has_gv && store->exists_bl_sn(machine_gv.c_str(), ver)) {
	stringstream s;
	s << ver;
	string ver_str = s.str();

	version_t gv = store->get_int(machine_gv.c_str(), ver_str.c_str());
	std::cerr << __func__ << " " << machine
		  << " ver " << ver << " -> " << gv << std::endl;

	bufferlist tx_bl;
	tx.encode(tx_bl);
	tx.put("paxos", gv, tx_bl);
      }
      db->apply_transaction(tx);
    }

    version_t lc = db->get(machine, "last_committed");
    assert(lc == last_committed);

    MonitorDBStore::Transaction tx;
    tx.put(machine, "first_committed", first_committed);
    tx.put(machine, "last_committed", last_committed);
    db->apply_transaction(tx);

    return 0;
  }

  int _convert_machines() {

    set<string> machine_names = _get_machines_names();
    set<string>::iterator it = machine_names.begin();

    std::cout << __func__ << std::endl;

    for (; it != machine_names.end(); ++it) {
      int r = _convert_machines(*it);
      assert(r == 0);
    }

    MonitorDBStore::Transaction tx;
    tx.put("paxos", "accepted_pn", highest_accepted_pn);
    tx.put("paxos", "last_pn", highest_last_pn);
    db->apply_transaction(tx);

    return 0;
  }
};


void usage(const char *pname)
{
  std::cerr << "Usage: " << pname << " <old store path>\n"
    << std::endl;
}

int main(int argc, const char *argv[])
{
  vector<const char*> def_args;
  vector<const char*> args;
  const char *our_name = argv[0];
  argv_to_vec(argc, argv, args);

  global_init(&def_args, args,
	      CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
	      CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf->apply_changes(NULL);

  if (args.size() < 1) {
    usage(our_name);
    return 1;
  }
  string store(args[0]);
  string new_store(store);
  MonitorStoreConverter converter(store, new_store);
  assert(!converter.convert());
  assert(converter.match());

  std::cout << "store successfully converted to new format" << std::endl;

  return 0;
}
