/////////////////////////////////////////////////////////////////////////////
//
// BSD 3-Clause License
//
// Copyright (c) 2019, University of California, San Diego.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "PartitionMgr.h"

#include "opendb/db.h"
#include "utl/Logger.h"
#include "ord/OpenRoad.hh"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"

#include "sta/MakeConcreteNetwork.hh"
#include "sta/VerilogWriter.hh"
#include "sta/PortDirection.hh"
#include "sta/ParseBus.hh"

#include <map>
#include <set>
#include <algorithm>

namespace par {

using utl::PAR;

using sta::Network;
using sta::ConcreteNetwork;
using sta::Library;
using sta::Cell;
using sta::Instance;
using sta::Net;
using sta::Port;
using sta::PortDirection;
using sta::writeVerilog;

using sta::isBusName;
using sta::parseBusName;

using sta::dbNetwork;

using odb::dbInst;
using odb::dbIntProperty;
using odb::dbBlock;
using odb::dbBTerm;
using odb::dbITerm;
using odb::dbNet;
using odb::dbIoType;

using sta::NetworkReader;
using sta::dbSta;

PortDirection*
determinePortDirection(dbNet* net, std::set<dbInst*>* insts) {
  bool local_only = true;
  bool locally_driven = false;
  bool externally_driven = false;

  for (dbBTerm* bterm : net->getBTerms()) {
    switch (bterm->getIoType()) {
    case dbIoType::INPUT:
      externally_driven = true;
      local_only = false;
      break;
    case dbIoType::INOUT:
      externally_driven = true;
      local_only = false;
      break;
    }
  }

  if (insts != nullptr) {
     for (dbITerm* iterm : net->getITerms()) {
      dbInst* inst = iterm->getInst();

      // check if instance is not present in the current partition, port will be needed
      if (insts->find(inst) == insts->end()) {
        local_only = false;
        // instance on other partition, so iterm has opposite direction
        switch (iterm->getIoType()) {
        case dbIoType::OUTPUT:
          externally_driven = true;
          break;
        case dbIoType::INOUT:
          externally_driven = true;
          break;
        }
      }
      else {
        // instance in partition, so iterm has correct port direction
        switch (iterm->getIoType()) {
        case dbIoType::OUTPUT:
          locally_driven = true;
          break;
        case dbIoType::INOUT:
          locally_driven = true;
          break;
        }
      }
    }
  }

  // no port is needed
  if (local_only)
    return nullptr;

  if (locally_driven && externally_driven) {
    return PortDirection::bidirect();
  } else if (externally_driven) {
    return PortDirection::input();
  } else { // internally driven
    return PortDirection::output();
  }
}

Instance*
PartitionMgr::buildPartitionedInstance(
    const char* name,
    const char* port_prefix,
    sta::Library* library,
    sta::NetworkReader* network,
    sta::Instance* parent,
    std::set<dbInst*>* insts,
    std::map<dbNet*, Port*>* port_map) {

  // setup access
  dbNetwork* db_network = ord::OpenRoad::openRoad()->getSta()->getDbNetwork();
  dbBlock* block = getDbBlock();

  // build cell
  Cell* cell = network->makeCell(library, name, false, nullptr);

  // add global ports
  for (dbBTerm* bterm : block->getBTerms()) {
    bool add_port = parent == nullptr; // add port if parent
    dbNet* net = bterm->getNet();
    if (net != nullptr && insts != nullptr) {
      for (dbITerm* iterm : bterm->getNet()->getITerms()) {
        if (add_port)
          break;

        // check if port is connected to instance in this partition
        if (insts->find(iterm->getInst()) != insts->end()) {
          add_port = true;
          break;
        }
      }
    }

    if (add_port) {
      std::string portname = bterm->getName();

      Port* port = network->makePort(cell, portname.c_str());
      // copy exactly the parent port direction
      network->setDirection(port, db_network->dbToSta(bterm->getSigType(), bterm->getIoType()));
      if (parent != nullptr) {
        PortDirection* sub_module_dir = determinePortDirection(bterm->getNet(), insts);
        if (sub_module_dir != nullptr)
          network->setDirection(port, sub_module_dir);
      }

      if (port_map != nullptr)
        port_map->insert({net, port});
    }
  }

  // make internal ports for partitions and if port is not needed.
  if (insts != nullptr) {
    for (dbInst* db_inst : *insts) {
      for (dbITerm* term : db_inst->getITerms()) {
        dbNet* db_net = term->getNet();
        if (db_net != nullptr && // connected
            port_map->find(db_net) == port_map->end()) {// port not present
          // check if connected to anything in a different partition
          bool added_internal_port = false;
          for (dbITerm* iterms : db_net->getITerms()) {
            if (added_internal_port)
              break;

            PortDirection* port_dir = determinePortDirection(db_net, insts);
            if (port_dir != nullptr) {
              std::string port_name = port_prefix;
              port_name += db_net->getName();

              Port* port = network->makePort(cell, port_name.c_str());
              network->setDirection(port, port_dir);

              port_map->insert({db_net, port});

              added_internal_port = true;
              break;
            }
          }
        }
      }
    }
  }

  if (parent != nullptr) {
    // loop over buses and to ensure all bit ports are created, only needed for partitioned modules
    char path_escape = network->pathEscape();
    char left_bracket = '['; // library->busBrktLeft();
    char right_bracket = ']'; // library->busBrktRight();
    std::map<std::string, std::vector<Port*>> port_buses;
    for (auto& [net, port] : *port_map) {
      std::string portname = network->name(port);

      // check if bus and get name
      if (isBusName(portname.c_str(), left_bracket, right_bracket, path_escape)) {
        char* bus_name;
        int idx;
        parseBusName(portname.c_str(), left_bracket, right_bracket, path_escape, bus_name, idx);
        portname = bus_name;
        delete bus_name;

        if (port_buses.find(portname) == port_buses.end()) {
          port_buses[portname] = std::vector<Port*>();
        }
        port_buses[portname].push_back(port);
      }
    }
    for (auto& [bus, ports] : port_buses) {
      std::set<int> port_idx;
      std::set<PortDirection*> port_dirs;
      for (Port* port : ports) {
        char* bus_name;
        int idx;
        parseBusName(network->name(port), left_bracket, right_bracket, path_escape, bus_name, idx);
        delete bus_name;

        port_idx.insert(idx);

        port_dirs.insert(network->direction(port));
      }

      // determine real direction of port
      PortDirection* overall_direction = nullptr;
      if (port_dirs.size() == 1) // only one direction is used.
        overall_direction = *port_dirs.begin();
      else
        overall_direction = PortDirection::bidirect();

      // set port direction to match
      for (Port* port : ports)
        network->setDirection(port, overall_direction);

      // fill in missing ports in bus
      const auto [min_idx, max_idx] = std::minmax_element(port_idx.begin(), port_idx.end());
      for (int idx = *min_idx; idx <= *max_idx; idx++) {
        if (port_idx.find(idx) == port_idx.end()) {
          // build missing port
          std::string portname = bus;
          portname += left_bracket + std::to_string(idx) + right_bracket;
          Port* port = network->makePort(cell, portname.c_str());
          network->setDirection(port, overall_direction);
        }
      }
    }
  }
  network->groupBusPorts(cell);

  // build instance
  std::string instname = name;
  instname += "_inst";
  Instance* inst = network->makeInstance(cell, instname.c_str(), parent);

  if (port_map != nullptr) {
    // create nets for ports in cell
    for (auto& [db_net, port] : *port_map)
      network->makeNet(network->name(port), inst);
  }

  if (insts != nullptr) {
    // create and connect instances
    for (dbInst* db_inst : *insts) {
      Instance* leaf_inst = network->makeInstance(db_network->dbToSta(db_inst->getMaster()),
                                                  db_inst->getName().c_str(),
                                                  inst);
      for (dbITerm* term : db_inst->getITerms()) {
        dbNet* db_net = term->getNet();
        if (db_net != nullptr) { // connected
          Port* port = db_network->dbToSta(term->getMTerm());

          // check if connected to a port
          auto port_find = port_map->find(db_net);
          if (port_find != port_map->end()) {
            Net* net = network->findNet(inst, network->name(port_find->second));
            network->connect(leaf_inst, port, net);
          }
          else {
            Net* net = network->findNet(inst, db_net->getName().c_str());
            if (net == nullptr)
              net = network->makeNet(db_net->getName().c_str(), inst);
            network->connect(leaf_inst, port, net);
          }
        }
      }
    }
  }

  return inst;
}

void PartitionMgr::writePartitionVerilog(const char* path,
                                         const char* port_prefix,
                                         const char* module_suffix) {
  dbBlock* block = getDbBlock();
  if (block == nullptr)
    return;

  _logger->report("Writing partition to verilog.");

  // build partition instance map
  std::map<long, std::set<dbInst*>> instance_map;
  for (dbInst* inst : block->getInsts()) {
    dbIntProperty* prop_id = dbIntProperty::find(inst, "partition_id");
    if (!prop_id) {
      _logger->warn(PAR, 15, "Property not found for inst {}", inst->getName());
    }
    else {
      long partition = prop_id->getValue();
      if (instance_map.find(partition) == instance_map.end())
        instance_map.emplace(partition, std::set<dbInst*>());

      instance_map[partition].insert(inst);
    }
  }

  // get top module name
  dbNetwork* db_network = ord::OpenRoad::openRoad()->getSta()->getDbNetwork();
  std::string top_name = db_network->name(db_network->topInstance());

  // create new network and library
  NetworkReader* network = sta::makeConcreteNetwork();
  Library* library = network->makeLibrary("Partitions", nullptr);

  // new top module
  Instance* top_inst = buildPartitionedInstance(top_name.c_str(),
                                                "", // no changes to port
                                                library,
                                                network,
                                                nullptr, // no parent
                                                nullptr,
                                                nullptr);

  // build submodule partitions
  std::map<long, Instance*> sta_instance_map;
  std::map<long, std::map<dbNet*, Port*>> sta_port_map;
  for (auto& [partition, instances] : instance_map) {
    std::string cell_name = top_name + module_suffix + std::to_string(partition);
    sta_port_map[partition] = std::map<dbNet*, Port*>();
    sta_instance_map[partition] = buildPartitionedInstance(cell_name.c_str(),
                                                           port_prefix,
                                                           library,
                                                           network,
                                                           top_inst,
                                                           &instances,
                                                           &sta_port_map[partition]);
  }

  // connect submodule partitions in new top module
  for (auto& [partition, instance] : sta_instance_map) {
    for (auto& [portnet, port] : sta_port_map[partition]) {
      const char* net_name = network->name(port);

      Net* net = network->findNet(top_inst, net_name);
      if (net == nullptr)
        net = network->makeNet(net_name, top_inst);

      network->connect(instance, port, net);
    }
  }

  reinterpret_cast<ConcreteNetwork*>(network)->setTopInstance(top_inst);

  writeVerilog(path, false, false, {}, network);

  delete network;
}

}  // namespace par
