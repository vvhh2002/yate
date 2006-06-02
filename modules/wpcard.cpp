/**
 * wpcard.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Wanpipe PRI cards signalling and data driver
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

#include <yatess7.h>

#ifdef _WINDOWS
#error This module is not for Windows
#else

extern "C" {

#define INVALID_HANDLE_VALUE (-1)
#define __LINUX__
#include <linux/if_wanpipe.h>
#include <linux/if.h>
#include <linux/wanpipe.h>
#include <linux/wanpipe_cfg.h>
#include <linux/sdla_bitstrm.h>

};

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <sys/ioctl.h>
#include <fcntl.h>

#define WP_HEADER 16

#define WP_RD_ERROR    0
#define WP_RD_STAMP_LO 1
#define WP_RD_STAMP_HI 2

#define WP_WR_TYPE     0
#define WP_WR_FORCE    1

#define WP_ERR_FIFO  0x01
#define WP_ERR_CRC   0x02
#define WP_ERR_ABORT 0x04

#define MAX_PACKET 1200

using namespace TelEngine;
namespace { // anonymous

class WpSigThread;

class WpInterface : public SignallingInterface
{
    friend class WpSigThread;
public:
    static void* create(const String& type, const NamedList& name);
    WpInterface(const char* card, const char* device);
    virtual ~WpInterface();
    virtual bool transmitPacket(const DataBlock& packet, bool repeat, PacketType type);
    virtual bool control(Operation oper, NamedList* params);
protected:
    virtual void timerTick(const Time& when);
    void receiveAttempt();
private:
    bool openSocket();
    Socket m_socket;
    String m_card;
    String m_device;
    WpSigThread* m_thread;
    bool m_received;
    int m_overRead;
};

class WpSigThread : public Thread
{
public:
    inline WpSigThread(WpInterface* iface)
	: m_interface(iface)
	{ }
    virtual ~WpSigThread();
    virtual void run();
private:
    WpInterface* m_interface;
};

YSIGFACTORY2(WpInterface,SignallingInterface);

//class WpSigFactory : public SignallingFactory

void* WpInterface::create(const String& type, const NamedList& name)
{
    if (type == "WpInterface") {
	WpInterface* iface = new WpInterface(name.getValue("card"),name.getValue("device"));
	iface->setName(name.getValue("name",type));
	return iface;
    }
    return 0;
}

WpInterface::WpInterface(const char* card, const char* device)
    : m_card(card), m_device(device), m_thread(0), m_received(false),
      m_overRead(3)
{
    Debug(DebugAll,"WpInterface::WpInterface('%s','%s') [%p]",
	card,device,this);
}

WpInterface::~WpInterface()
{
    Debug(DebugAll,"WpInterface::~WpInterface() [%p]",this);
    if (m_thread) {
	m_thread->cancel();
	while (m_thread)
	    Thread::yield();
//	m_thread = 0;
    }
    m_socket.terminate();
}

bool WpInterface::transmitPacket(const DataBlock& packet, bool repeat, PacketType type)
{
    if (!m_socket.valid())
	return false;

#ifdef XDEBUG
    if (debugAt(DebugAll)) {
	const char hex[] = "0123456789abcdef";
	unsigned char* s = (unsigned char*) packet.data();
	unsigned int len = packet.length();
	String str(' ',3*len);
	char* d = (char*) str.c_str();
	for (unsigned int i = 0; i < len; i++) {
	    unsigned char c = *s++;
	    *d++ = ' ';
	    *d++ = hex[(c >> 4) & 0x0f];
	    *d++ = hex[c & 0x0f];
	}
	Debug(toString(),DebugAll,"Sending %u bytes:%s",packet.length(),str.c_str());
    }
#endif

    int sz = WP_HEADER + packet.length();
    DataBlock data(0,WP_HEADER);
    data += packet;
    unsigned char* d = static_cast<unsigned char*>(data.data());
    if (repeat)
	d[WP_WR_FORCE] = 1;
    switch (type) {
	case SS7Fisu:
	    d[WP_WR_TYPE] = WANOPT_SS7_FISU;
	    break;
	case SS7Lssu:
	    d[WP_WR_TYPE] = WANOPT_SS7_LSSU;
	    break;
	default:
	    break;
    }
    int w = m_socket.send(data.data(),data.length());
    if (Socket::socketError() == w) {
	DDebug(toString(),DebugWarn,"Error on sending packet of %u bytes: %d: %s [%p]",
	    packet.length(),m_socket.error(),::strerror(m_socket.error()),this);
	return false;
    }
    if (w != sz) {
	DDebug(toString(),DebugWarn,"Sent %d instead of %d bytes [%p]",w,sz,this);
	return false;
    }
    w -= WP_HEADER;
    XDebug(toString(),DebugAll,"Successfully sent %d bytes packet [%p]",w,this);
    return true;
}

void WpInterface::receiveAttempt()
{
    if (!m_socket.valid())
	return;
    unsigned char buf[WP_HEADER + MAX_PACKET];
    int r = m_socket.recv(buf,sizeof(buf));
    if (Socket::socketError() == r) {
	if (m_socket.canRetry())
	    return;
	DDebug(toString(),DebugWarn,"Error on reading packet: %d: %s [%p]",
	    m_socket.error(),::strerror(m_socket.error()),this);
	return;
    }
    if (r > (WP_HEADER + m_overRead)) {
	r -= (WP_HEADER + m_overRead);
	XDebug(toString(),DebugAll,"Received %d bytes packet [%p]",r,this);
	if (buf[WP_RD_ERROR]) {
	    DDebug(toString(),DebugWarn,"Packet got error: %u [%p]",
		buf[WP_RD_ERROR],this);
	    if (buf[WP_RD_ERROR] & WP_ERR_FIFO)
		notify(RxOverflow);
	    if (buf[WP_RD_ERROR] & WP_ERR_CRC)
		notify(CksumError);
	    if (buf[WP_RD_ERROR] & WP_ERR_ABORT)
		notify(AlignError);
	    return;
	}

#ifdef XDEBUG
    if (debugAt(DebugAll)) {
	const char hex[] = "0123456789abcdef";
	unsigned char* s = buf+WP_HEADER;
	String str(' ',3*r);
	char* d = (char*) str.c_str();
	for (unsigned int i = 0; i < (unsigned int)r; i++) {
	    unsigned char c = *s++;
	    *d++ = ' ';
	    *d++ = hex[(c >> 4) & 0x0f];
	    *d++ = hex[c & 0x0f];
	}
	Debug(toString(),DebugAll,"Received %d bytes:%s",r,str.c_str());
    }
#endif

	m_received = true;
	DataBlock data(buf+WP_HEADER,r);
	receivedPacket(data);
	data.clear(false);
    }
}

bool WpInterface::openSocket()
{
    Debug(DebugAll,"WpInterface::openSocket() [%p]",this);
    if (!m_socket.create(AF_WANPIPE,SOCK_RAW)) {
	Debug(DebugGoOn,"Wanpipe failed to create socket, error %d: %s",
	    m_socket.error(),::strerror(m_socket.error()));
	return false;
    }
    // Bind to the card/interface
    struct wan_sockaddr_ll sa;
    memset(&sa,0,sizeof(struct wan_sockaddr_ll));
    ::strncpy((char*)sa.sll_device,m_device.safe(),sizeof(sa.sll_device));
    ::strncpy((char*)sa.sll_card,m_card.safe(),sizeof(sa.sll_card));
    sa.sll_protocol = htons(PVC_PROT);
    sa.sll_family=AF_WANPIPE;
    if (!m_socket.bind((struct sockaddr *)&sa, sizeof(sa))) {
	Debug(DebugGoOn,"Wanpipe failed to bind socket, error %d: %s",
	    m_socket.error(),::strerror(m_socket.error()));
	m_socket.terminate();
	return false;
    }
    if (!m_socket.setBlocking(false)) {
	Debug(DebugGoOn,"Wanpipe failed to set socket non-blocking, error %d: %s",
	    m_socket.error(),::strerror(m_socket.error()));
	m_socket.terminate();
	return false;
    }
    return true;
}

bool WpInterface::control(Operation oper, NamedList* params)
{
    switch (oper) {
	case Enable:
	    if (!(m_socket.valid() || openSocket()))
		return false;
	    if (!m_thread)
		m_thread = new WpSigThread(this);
	    return m_thread->startup();
	case Query:
	    return m_socket.valid() && m_thread && m_thread->running();
	default:
	    return SignallingInterface::control(oper,params);
    }
}

void WpInterface::timerTick(const Time& when)
{
    if (m_received)
	m_received = false;
    else {
	XDebug(toString(),DebugAll,"Not received any packets in the last tick [%p]",this);
    }
}


WpSigThread::~WpSigThread()
{
    Debug(DebugAll,"WpSigThread::~WpSigThread() [%p]",this);
    if (m_interface)
	m_interface->m_thread = 0;
}

void WpSigThread::run()
{
    Debug(DebugAll,"WpSigThread::run() [%p]",this);
    for (;;) {
	Thread::yield(true);
	if (m_interface)
	    m_interface->receiveAttempt();
    }
}

#if 0
class WpChan;

class WpSpan : public PriSpan, public Thread
{
    friend class WpData;
    friend class WpDriver;
public:
    virtual ~WpSpan();
    virtual void run();
    inline int overRead() const
	{ return m_overRead; }

private:
    WpSpan(struct pri *_pri, PriDriver* driver, int span, int first, int chans, int dchan, Configuration& cfg, const String& sect, HANDLE fd);
    HANDLE m_fd;
    WpData *m_data;
    int m_overRead;
};

class WpChan : public PriChan
{
    friend class WpSource;
    friend class WpConsumer;
    friend class WpData;
public:
    WpChan(const PriSpan *parent, int chan, unsigned int bufsize);
    virtual ~WpChan();
    virtual bool openData(const char* format, int echoTaps);

private:
    WpSource* m_wp_s;
    WpConsumer* m_wp_c;
};

class WpData : public Thread
{
public:
    WpData(WpSpan* span, const char* card, const char* device, Configuration& cfg, const String& sect);
    ~WpData();
    virtual void run();
private:
    WpSpan* m_span;
    HANDLE m_fd;
    unsigned char* m_buffer;
    WpChan **m_chans;
    int m_samples;
    bool m_swap;
    unsigned char m_rdError;
    unsigned char m_wrError;
};

#define MAX_DATA_ERRORS 250

static int wp_recv(HANDLE fd, void *buf, int buflen, int flags = 0)
{
    int r = ::recv(fd,buf,buflen,flags);
    return r;
}

static int wp_send(HANDLE fd, void *buf, int buflen, int flags = 0)
{
    int w = ::send(fd,buf,buflen,flags);
    return w;
}

static int wp_read(struct pri *pri, void *buf, int buflen)
{
    buflen -= 2;
    int sz = buflen+WP_HEADER;
    char *tmp = (char*)::calloc(sz,1);
    XDebug("wp_read",DebugAll,"pre buf=%p len=%d tmp=%p sz=%d",buf,buflen,tmp,sz);
    int r = wp_recv((HANDLE)::pri_fd(pri),tmp,sz,MSG_NOSIGNAL);
    XDebug("wp_read",DebugAll,"post r=%d",r);
    if (r > 0) {
	r -= WP_HEADER;
	if ((r > 0) && (r <= buflen)) {
	    WpSpan* span = (WpSpan*)::pri_get_userdata(pri);
	    if (span)
		r -= span->overRead();
	    DDebug("wp_read",DebugAll,"Transferring %d for %p",r,pri);
	    ::memcpy(buf,tmp+WP_HEADER,r);
	    r += 2;
	}
    }
    ::free(tmp);
    return r;
}

static int wp_write(struct pri *pri, void *buf, int buflen)
{
    buflen -= 2;
    int sz = buflen+WP_HEADER;
    char *tmp = (char*)::calloc(sz,1);
    ::memcpy(tmp+WP_HEADER,buf,buflen);
    XDebug("wp_write",DebugAll,"pre buf=%p len=%d tmp=%p sz=%d",buf,buflen,tmp,sz);
    int w = wp_send((HANDLE)::pri_fd(pri),tmp,sz,0);
    XDebug("wp_write",DebugAll,"post w=%d",w);
    if (w > 0) {
	w -= WP_HEADER;
	DDebug("wp_write",DebugAll,"Transferred %d for %p",w,pri);
	w += 2;
    }
    ::free(tmp);
    return w;
}



static bool wp_select(HANDLE fd,int samp,bool* errp = 0)
{
    fd_set rdfds;
    fd_set errfds;
    FD_ZERO(&rdfds);
    FD_SET(fd,&rdfds);
    FD_ZERO(&errfds);
    FD_SET(fd,&errfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = samp*125;
    int sel = ::select(fd+1, &rdfds, NULL, errp ? &errfds : NULL, &tv);
    if (sel < 0)
	Debug(DebugWarn,"Wanpipe select failed on %d: error %d: %s",
	    fd,errno,::strerror(errno));
    if (errp)
	*errp = FD_ISSET(fd,&errfds);
    return FD_ISSET(fd,&rdfds);
}

void wp_close(HANDLE fd)
{
    if (fd == INVALID_HANDLE_VALUE)
	return;
    ::close(fd);
}

static HANDLE wp_open(const char* card, const char* device)
{
    DDebug(DebugAll,"wp_open('%s','%s')",card,device);
    if (null(card) || null(device))
	return INVALID_HANDLE_VALUE;
    HANDLE fd = ::socket(AF_WANPIPE, SOCK_RAW, 0);
    if (fd == INVALID_HANDLE_VALUE) {
	Debug(DebugGoOn,"Wanpipe failed to create socket: error %d: %s",
	    errno,::strerror(errno));
	return fd;
    }
    // Bind to the card/interface
    struct wan_sockaddr_ll sa;
    memset(&sa,0,sizeof(struct wan_sockaddr_ll));
    ::strncpy((char*)sa.sll_device,device,sizeof(sa.sll_device));
    ::strncpy((char*)sa.sll_card,card,sizeof(sa.sll_card));
    sa.sll_protocol = htons(PVC_PROT);
    sa.sll_family=AF_WANPIPE;
    if (::bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
	Debug(DebugGoOn,"Wanpipe failed to bind %d: error %d: %s",
	    fd,errno,::strerror(errno));
	wp_close(fd);
	fd = INVALID_HANDLE_VALUE;
    }
    return fd;
}

static struct pri* wp_create(const char* card, const char* device, int nettype, int swtype)
{
    DDebug(DebugAll,"wp_create('%s','%s',%d,%d)",card,device,nettype,swtype);
    HANDLE fd = wp_open(card,device);
    if (fd == INVALID_HANDLE_VALUE)
	return 0;
    struct pri* p = ::pri_new_cb((int)fd, nettype, swtype, wp_read, wp_write, 0);
    if (!p)
	wp_close(fd);
    return p;
}

WpSpan::WpSpan(struct pri *_pri, PriDriver* driver, int span, int first, int chans, int dchan, Configuration& cfg, const String& sect, HANDLE fd)
    : PriSpan(_pri,driver,span,first,chans,dchan,cfg,sect), Thread("WpSpan"),
      m_fd(fd), m_data(0), m_overRead(0)
{
    Debug(&__plugin,DebugAll,"WpSpan::WpSpan() [%p]",this);
    m_overRead = cfg.getIntValue(sect,"overread",cfg.getIntValue("general","overread",0));
}

WpSpan::~WpSpan()
{
    Debug(&__plugin,DebugAll,"WpSpan::~WpSpan() [%p]",this);
    m_ok = false;
    delete m_data;
    wp_close(m_fd);
    m_fd = INVALID_HANDLE_VALUE;
}

void WpSpan::run()
{
    Debug(&__plugin,DebugAll,"WpSpan::run() [%p]",this);
    for (;;) {
	bool rd = wp_select(m_fd,5); // 5 bytes per smallest q921 frame
	Thread::check();
	runEvent(!rd);
    }
}

WpSource::WpSource(WpChan *owner, const char* format, unsigned int bufsize)
    : PriSource(owner,format,bufsize),
      m_bufpos(0)
{
    Debug(m_owner,DebugAll,"WpSource::WpSource(%p) [%p]",owner,this);
    static_cast<WpChan*>(m_owner)->m_wp_s = this;
}

WpSource::~WpSource()
{
    Debug(m_owner,DebugAll,"WpSource::~WpSource() [%p]",this);
    static_cast<WpChan*>(m_owner)->m_wp_s = 0;
}

void WpSource::put(unsigned char val)
{
    ((char*)m_buffer.data())[m_bufpos] = val;
    if (++m_bufpos >= m_buffer.length()) {
	m_bufpos = 0;
	Forward(m_buffer);
    }
}

WpConsumer::WpConsumer(WpChan *owner, const char* format, unsigned int bufsize)
    : PriConsumer(owner,format,bufsize), Fifo(2*bufsize)
{
    Debug(m_owner,DebugAll,"WpConsumer::WpConsumer(%p) [%p]",owner,this);
    static_cast<WpChan*>(m_owner)->m_wp_c = this;
}

WpConsumer::~WpConsumer()
{
    Debug(m_owner,DebugAll,"WpConsumer::~WpConsumer() [%p]",this);
    static_cast<WpChan*>(m_owner)->m_wp_c = 0;
    if (m_overruns.events())
	Debug(m_owner,DebugMild,"Consumer had %u overruns (%lu bytes)",
	    m_overruns.events(),m_overruns.bytes());
}

void WpConsumer::Consume(const DataBlock &data, unsigned long tStamp)
{
    unsigned int err = put((const unsigned char*)data.data(),data.length());
    if (err)
	m_overruns.update(err);
}

static Thread::Priority cfgPriority(Configuration& cfg, const String& sect)
{
    String tmp(cfg.getValue(sect,"thread"));
    if (tmp.null())
	tmp = cfg.getValue("general","thread");
    return Thread::priority(tmp);
}

WpData::WpData(WpSpan* span, const char* card, const char* device, Configuration& cfg, const String& sect)
    : Thread("WpData",cfgPriority(cfg,sect)), m_span(span), m_fd(INVALID_HANDLE_VALUE),
      m_buffer(0), m_chans(0), m_samples(50), m_swap(true), m_rdError(0), m_wrError(0)
{
    Debug(&__plugin,DebugAll,"WpData::WpData(%p,'%s','%s') [%p]",
	span,card,device,this);
    HANDLE fd = wp_open(card,device);
    if (fd != INVALID_HANDLE_VALUE) {
	m_fd = fd;
	m_span->m_data = this;
    }

    if (m_span->chans() == 24)
	// for T1 we typically have 23 B channels so we adjust the number
	//  of samples to get multiple of 32 bit and also reduce overhead
	m_samples = 64;

    m_samples = cfg.getIntValue("general","samples",m_samples);
    m_samples = cfg.getIntValue(sect,"samples",m_samples);
    m_swap = cfg.getBoolValue("general","bitswap",m_swap);
    m_swap = cfg.getBoolValue(sect,"bitswap",m_swap);
}

WpData::~WpData()
{
    Debug(&__plugin,DebugAll,"WpData::~WpData() [%p]",this);
    m_span->m_data = 0;
    wp_close(m_fd);
    m_fd = INVALID_HANDLE_VALUE;
    if (m_buffer)
	::free(m_buffer);
    if (m_chans)
	delete[] m_chans;
}

void WpData::run()
{
    Debug(&__plugin,DebugAll,"WpData::run() [%p]",this);
    int bchans = m_span->bchans();
    int buflen = m_samples*bchans;
    int sz = buflen+WP_HEADER;
    m_buffer = (unsigned char*)::malloc(sz);
    // Build a compacted list of allocated B channels
    m_chans = new WpChan* [bchans];
    int b = 0;
    for (int n = 0; n < bchans; n++) {
	while (!m_span->m_chans[b])
	    b++;
	m_chans[n] = static_cast<WpChan*>(m_span->m_chans[b++]);
	DDebug(&__plugin,DebugInfo,"wpdata ch[%d]=%d (%p)",n,m_chans[n]->chan(),m_chans[n]);
    }
    while (m_span && (m_fd >= 0)) {
	Thread::check();
	bool oob = false;
	bool rd = wp_select(m_fd,m_samples,&oob);
	if (oob) {
	    XDebug("wpdata_recv_oob",DebugAll,"pre buf=%p len=%d sz=%d",m_buffer,buflen,sz);
	    int r = wp_recv(m_fd,m_buffer,sz,MSG_OOB);
	    XDebug("wpdata_recv_oob",DebugAll,"post r=%d",r);
	    if (r > 0)
		Debug(&__plugin,DebugInfo,"Read %d bytes of OOB data on span %d [%p]",
		    r,m_span->span(),this);
	}

	if (rd) {
	    m_buffer[0] = 0;
	    XDebug("wpdata_recv",DebugAll,"pre buf=%p len=%d sz=%d",m_buffer,buflen,sz);
	    int r = wp_recv(m_fd,m_buffer,sz,0/*MSG_NOSIGNAL*/);
	    XDebug("wpdata_recv",DebugAll,"post r=%d",r);
	    r -= WP_HEADER;
	    if (m_buffer[0]) {
		if (!m_rdError)
		    Debug(&__plugin,DebugWarn,"Read data error 0x%02X on span %d [%p]",
			m_buffer[0],m_span->span(),this);
		if (m_rdError < MAX_DATA_ERRORS)
		    m_rdError++;
	    }
	    else
		m_rdError = 0;
	    // We should have read N bytes for each B channel
	    if ((r > 0) && ((r % bchans) == 0)) {
		r /= bchans;
		const unsigned char* dat = m_buffer + WP_HEADER;
		m_span->lock();
		for (int n = r; n > 0; n--)
		    for (b = 0; b < bchans; b++) {
			WpSource *s = m_chans[b]->m_wp_s;
			if (s)
			    s->put(m_swap ? PriDriver::bitswap(*dat) : *dat);
			dat++;
		    }
		m_span->unlock();
	    }
	    int wr = m_samples;
	    ::memset(m_buffer,0,WP_HEADER);
	    unsigned char* dat = m_buffer + WP_HEADER;
	    m_span->lock();
	    for (int n = wr; n > 0; n--) {
		for (b = 0; b < bchans; b++) {
		    WpConsumer *c = m_chans[b]->m_wp_c;
		    unsigned char d = c ? c->get() : 0xff;
		    *dat++ = m_swap ? PriDriver::bitswap(d) : d;
		}
	    }
	    m_span->unlock();
	    wr = (wr * bchans) + WP_HEADER;
	    XDebug("wpdata_send",DebugAll,"pre buf=%p len=%d sz=%d",m_buffer,wr,sz);
	    int w = wp_send(m_fd,m_buffer,wr,MSG_DONTWAIT);
	    XDebug("wpdata_send",DebugAll,"post w=%d",w);
	    if (w != wr) {
		if (!m_wrError)
		    Debug(&__plugin,DebugWarn,"Wrote %d data bytes instead of %d on span %d [%p]",
			w,wr,m_span->span(),this);
		if (m_wrError < MAX_DATA_ERRORS)
		    m_wrError++;
	    }
	    else
		m_wrError = 0;
	}
    }
}

WpChan::WpChan(const PriSpan *parent, int chan, unsigned int bufsize)
    : PriChan(parent,chan,bufsize), m_wp_s(0), m_wp_c(0)
{
}

WpChan::~WpChan()
{
    closeData();
}

bool WpChan::openData(const char* format, int echoTaps)
{
    if (echoTaps)
	Debug(DebugWarn,"Echo cancellation requested but not available in wanpipe");
    m_span->lock();
    setSource(new WpSource(this,format,m_bufsize));
    getSource()->deref();
    setConsumer(new WpConsumer(this,format,m_bufsize));
    getConsumer()->deref();
    m_span->unlock();
    return true;
}

PriSpan* WpDriver::createSpan(PriDriver* driver, int span, int first, int chans, Configuration& cfg, const String& sect)
{
    Debug(this,DebugAll,"WpDriver::createSpan(%p,%d,%d,%d) [%p]",driver,span,first,chans,this);
    int netType = -1;
    int swType = -1;
    int dchan = -1;
    netParams(cfg,sect,chans,&netType,&swType,&dchan);
    String card;
    card << "wanpipe" << span;
    card = cfg.getValue(sect,"card",card);
    String dev;
    dev << "w" << span << "g1";
    pri* p = wp_create(card,cfg.getValue(sect,"dgroup",dev),netType,swType);
    if (!p)
	return 0;
    WpSpan *ps = new WpSpan(p,driver,span,first,chans,dchan,cfg,sect,(HANDLE)::pri_fd(p));
    ps->startup();
    dev.clear();
    dev << "w" << span << "g2";
    WpData* dat = new WpData(ps,card,cfg.getValue(sect,"bgroup",dev),cfg,sect);
    dat->startup();
    return ps;
}

PriChan* WpDriver::createChan(const PriSpan* span, int chan, unsigned int bufsize)
{
    Debug(this,DebugAll,"WpDriver::createChan(%p,%d,%u) [%p]",span,chan,bufsize,this);
    return new WpChan(span,chan,bufsize);
}

WpDriver::WpDriver()
    : PriDriver("wp")
{
    Output("Loaded module Wanpipe");
}

WpDriver::~WpDriver()
{
    Output("Unloading module Wanpipe");
}

void WpDriver::initialize()
{
    Output("Initializing module Wanpipe");
    init("wpchan");
}
#endif // 0

}; // anonymous namespace

#endif /* _WINDOWS */

/* vi: set ts=8 sw=4 sts=4 noet: */
