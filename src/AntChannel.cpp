// -*- mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2; coding: utf-8-unix -*-
// ***** BEGIN LICENSE BLOCK *****
////////////////////////////////////////////////////////////////////
//                                                                //
// Copyright (c) 2011-2013 RALOVICH, Kristóf                      //
//                                                                //
// This program is free software; you can redistribute it and/or  //
// modify it under the terms of the GNU General Public License    //
// version 2 as published by the Free Software Foundation.        //
//                                                                //
////////////////////////////////////////////////////////////////////
// ***** END LICENSE BLOCK *****


#include "AntChannel.hpp"
#include <cstdio>



namespace antpm{

void
AntChannel::addMsgListener2(AntListenerBase* lb)
{
  boost::unique_lock<boost::mutex> lock(m_mtxListeners);
  listeners.push_back(lb);
}

void
AntChannel::rmMsgListener2(AntListenerBase* lb)
{
  boost::unique_lock<boost::mutex> lock(m_mtxListeners);
  listeners.remove(lb);
}

void
AntChannel::onMsg(AntMessage &m)
{
  boost::unique_lock<boost::mutex> lock(m_mtxListeners);
  for(std::list<AntListenerBase*>::iterator i = listeners.begin(); i != listeners.end(); i++)
  {
    (*i)->onMsg(m);
  }
}






AntListenerBase::AntListenerBase(AntChannel& o)
  : owner(o)
{
  owner.addMsgListener2(this);
}


AntListenerBase::~AntListenerBase()
{
  owner.rmMsgListener2(this);
}


void
AntListenerBase::onMsg(AntMessage& m)
{
  boost::unique_lock<boost::mutex> lock(m_mtxResp);
  if(match(m))
  {
    //assert(!m_msgResp); //this assert won't always work: e.g. if the waitForMsg() already concluded, there is noone to reset this pointer
    m_msgResp.reset(new AntMessage(m));
    m_cndResp.notify_all();
  }
}

bool
AntListenerBase::waitForMsg(AntMessage* m, const size_t timeout_ms)
{
  boost::unique_lock<boost::mutex> lock(m_mtxResp);
  if(m_msgResp)
  {
    if(m) *m = *m_msgResp; // it had already arrived
    m_msgResp.reset();
    return true;
  }
  if(!m_cndResp.timed_wait(lock, boost::posix_time::milliseconds(timeout_ms)))
  {
    return false;
  }
  assert(m_msgResp);
  if(m) *m=*m_msgResp;//copy
  m_msgResp.reset();
  return true;
}








bool
AntEvListener::match(AntMessage& other) const
{
  return other.getMsgId()==MESG_RESPONSE_EVENT_ID
      && owner.chan == other.getPayloadRef()[0]
      && other.getPayloadRef()[1]==MESG_EVENT_ID;
}
// whether there was a response before timeout
bool
AntEvListener::waitForEvent(uint8_t& msgCode, const size_t timeout_ms)
{
  AntMessage resp;
  if(!waitForMsg(&resp, timeout_ms))
    return false;
  msgCode = resp.getPayloadRef()[2];
  return true;
}








bool
AntRespListener::match(AntMessage& other) const
{
  return other.getMsgId()==MESG_RESPONSE_EVENT_ID
      && owner.chan == other.getPayloadRef()[0]
      && other.getPayloadRef()[1]==msgId;
}

// whether there was a response before timeout
bool
AntRespListener::waitForResponse(uint8_t& respVal, const size_t timeout_ms)
{
  AntMessage resp;
  if(!waitForMsg(&resp, timeout_ms))
    return false;
  respVal = resp.getPayloadRef()[2];
  return true;
}








bool
AntReqListener::match(AntMessage& other) const
{
  bool matched = other.getMsgId()==msgId
    && other.getPayloadRef()[0]==chan;
  //printf("matched=%d\n", int(matched));fflush(stdout);
  return matched;
}






bool
AntBCastListener::match(AntMessage& other) const
{
  return other.getMsgId()==MESG_BROADCAST_DATA_ID
      && other.getPayloadRef()[0]==owner.chan
      && other.getPayloadRef()[1]==first;
}

bool
AntBCastListener::waitForBCast(AntMessage& bcast, const size_t timeout_ms)
{
  return waitForMsg(&bcast, timeout_ms);
}















void
AntBurstListener::onMsg(AntMessage& m)
  {
    boost::unique_lock<boost::mutex> lock(m_mtxResp);
    if(match(m))
    {
      bursts.push_back(m);
      m_cndResp.notify_all();
    }
  }


bool
AntBurstListener::match(AntMessage& other) const
  {
    const M_ANT_Burst* burst(reinterpret_cast<const M_ANT_Burst*>(other.getPayloadRef()));
    return other.getMsgId()==MESG_BURST_DATA_ID
        && burst->chan==owner.chan;
  }


bool
AntBurstListener::waitForBursts(std::list<AntMessage>& bs, const size_t timeout_ms)
  {
    bs.clear();
    boost::unique_lock<boost::mutex> lock(m_mtxResp);
    if(!bursts.empty()) // some had already arrived
    {
      std::swap(bs, bursts);
      return true;
    }
    // TODO: handle arrival of event:EVENT_RX_FAIL
    if(!m_cndResp.timed_wait(lock, boost::posix_time::milliseconds(timeout_ms)))
      return false;
    assert(!bursts.empty());
    std::swap(bs, bursts);
    return true;
  }

bool
AntBurstListener::collectBurst(std::vector<uint8_t>& burstData, const size_t timeout_ms)
  {
    boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();
    boost::posix_time::ptime end = start+boost::posix_time::milliseconds(timeout_ms);

    boost::posix_time::ptime after=start;
    boost::posix_time::time_duration remaining = end-after;
    uchar expectedSeq=0;
    bool found=false;
    bool lastFound=false;
    while(remaining>boost::posix_time::time_duration(boost::posix_time::milliseconds(0)))
    {
      std::list<AntMessage> msgs;
      size_t left_ms=static_cast<size_t>(remaining.total_milliseconds());
      //printf("msg wait ms=%d\n",  int(left_ms));
      CHECK_RETURN_FALSE(waitForBursts(msgs, left_ms));
      after = boost::posix_time::microsec_clock::local_time();
      remaining = end-after;
      for(std::list<AntMessage>::iterator i = msgs.begin(); i != msgs.end(); i++)
      {
        AntMessage& repl(*i);
        CHECK_RETURN_FALSE(repl.getLenPayload()==9);
        const M_ANT_Burst* burst(reinterpret_cast<const M_ANT_Burst*>(repl.getPayloadRef()));
        //if(burst->chan != chan) // this check is a duplicate
        //{
        //  //++i;j++;
        //  continue;
        //}
        //LOG_VAR2((int)burst->seq, (int)expectedSeq);
        CHECK_RETURN_FALSE(burst->seq == expectedSeq);
        ++expectedSeq;
        if(expectedSeq == 4)
          expectedSeq = 1;
        found = true;
        std::vector<uchar> crtBurst(repl.getPayload());
        //std::string desc=burst->toString();
        //printf("reading %d bytes of BURST:%s\n", int(crtBurst.size()), desc.c_str());
        burstData.insert(burstData.end(), crtBurst.begin()+1,crtBurst.end());
        assert(burstData.size()%8==0);
        lastFound = burst->isLast();
        //i=m_rpackQueue.erase(i);j++;
        //if(burst->isLast())
        //{
        //  lastFound = true;
        //  //break;
        //}
        //LOG_VAR(lastFound);
      }
      if(lastFound)
      {
        //LOG_VAR(lastFound);
        break;
      }
    }
    if(!found || !lastFound)
    {
      lprintf(LOG_ERR, "couldn't reconstruct burst data transmission before timeout\n"); fflush(stdout);
      return false;
    }
    lprintf(LOG_INF, "collectBurst: %d bytes\n", int(burstData.size()));
    return true;
  }

}