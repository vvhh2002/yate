/**
 * pbx.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Basic PBX message handlers
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <yatephone.h>

using namespace TelEngine;
namespace { // anonymous

// chan.connect handler used to connect two channels
class ConnHandler : public MessageHandler
{
public:
    ConnHandler(int prio = 90)
	: MessageHandler("chan.connect",prio)
	{ }
    virtual bool received(Message &msg);
};

// call.execute handler used to 'steal' a channel
class ChanPickup : public MessageHandler
{
public:
    ChanPickup(int prio = 100)
	: MessageHandler("call.execute",prio)
	{ }
    virtual bool received(Message &msg);
};

class PbxModule : public Module
{
public:
    PbxModule();
    virtual ~PbxModule();
    virtual void initialize();
    bool m_first;
};


static PbxModule s_module;


// Utility function to get a pointer to a call endpoint (or its peer) by id
static CallEndpoint* locateChan(const String& id, bool peer = false)
{
    if (id.null())
	return 0;
    Message m("chan.locate");
    m.addParam("id",id);
    if (!Engine::dispatch(m))
	return 0;
    CallEndpoint* ce = static_cast<CallEndpoint*>(m.userObject("CallEndpoint"));
    if (!ce)
	return 0;
    return peer ? ce->getPeer() : ce;
}


bool ConnHandler::received(Message &msg)
{
    RefPointer<CallEndpoint> c1(locateChan(msg.getValue("id"),msg.getBoolValue("id_peer")));
    RefPointer<CallEndpoint> c2(locateChan(msg.getValue("targetid"),msg.getBoolValue("targetid_peer")));
    if (!(c1 && c2))
	return false;
    return c1->connect(c2,msg.getValue("reason"));
}


// call.execute handler used to 'steal' a channel
bool ChanPickup::received(Message& msg)
{
    String callto = msg.getValue("callto");
    if (!(callto.startSkip("pickup/",false) && callto))
	return false;

    // It's ours. Get the channels
    RefPointer<CallEndpoint> caller(static_cast<CallEndpoint*>(msg.userData()));
    RefPointer<CallEndpoint> called(locateChan(callto,true));

    if (!caller) {
	Debug(&s_module,DebugNote,"No channel to pick up: callto='%s'",
	    msg.getValue("callto"));
	msg.setParam("error","failure");
	return false;
    }
    if (!called) {
	Debug(&s_module,DebugInfo,
	    "Can't locate the peer for channel '%s' to pick up",callto.c_str());
	msg.setParam("error","nocall");
	return false;
    }

    // Connect parties and answer them
    if (!called->connect(caller,msg.getValue("reason","pickup"))) {
	Debug(&s_module,DebugNote,"Pick up failed to connect '%s' to '%s'",
	    caller->id().c_str(),called->id().c_str());
	return false;
    }

    Message* m = new Message("chan.masquerade");
    m->addParam("id",caller->id());
    m->addParam("message","call.answered");
    Engine::enqueue(m);
    m = new Message("chan.masquerade");
    m->addParam("id",called->id());
    m->addParam("message","call.answered");
    Engine::enqueue(m);
    return true;
}


PbxModule::PbxModule()
    : Module("pbx","misc"), m_first(true)
{
    Output("Loaded module PBX");
}

PbxModule::~PbxModule()
{
    Output("Unloading module PBX");
}

void PbxModule::initialize()
{
    Output("Initializing module PBX");
    if (m_first) {
	setup();
	m_first = false;
	Engine::install(new ConnHandler);
	Engine::install(new ChanPickup);
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
