#include <llarp/path.hpp>
#include <llarp/routing/handler.hpp>
#include <llarp/messages/discard.hpp>
#include "buffer.hpp"
#include "router.hpp"

namespace llarp
{
  namespace path
  {
    TransitHop::TransitHop()
    {
    }

    bool
    TransitHop::Expired(llarp_time_t now) const
    {
      return now > ExpireTime();
    }

    llarp_time_t
    TransitHop::ExpireTime() const
    {
      return started + lifetime;
    }

    TransitHopInfo::TransitHopInfo(const TransitHopInfo& other)
        : txID(other.txID)
        , rxID(other.rxID)
        , upstream(other.upstream)
        , downstream(other.downstream)
    {
    }

    TransitHopInfo::TransitHopInfo(const RouterID& down,
                                   const LR_CommitRecord& record)
        : txID(record.txid)
        , rxID(record.rxid)
        , upstream(record.nextHop)
        , downstream(down)
    {
    }

    TransitHop::TransitHop(const TransitHop& other)
        : info(other.info)
        , pathKey(other.pathKey)
        , started(other.started)
        , lifetime(other.lifetime)
        , version(other.version)
    {
    }

    bool
    TransitHop::SendRoutingMessage(llarp::routing::IMessage* msg,
                                   llarp_router* r)
    {
      if(!IsEndpoint(r->pubkey()))
        return false;
      byte_t tmp[MAX_LINK_MSG_SIZE - 128];
      auto buf = llarp::StackBuffer< decltype(tmp) >(tmp);
      if(!msg->BEncode(&buf))
      {
        llarp::LogError("failed to encode routing message");
        return false;
      }
      TunnelNonce N;
      N.Randomize();
      buf.sz = buf.cur - buf.base;
      // pad to nearest MESSAGE_PAD_SIZE bytes
      auto dlt = buf.sz % MESSAGE_PAD_SIZE;
      if(dlt)
      {
        dlt = MESSAGE_PAD_SIZE - dlt;
        // randomize padding
        r->crypto.randbytes(buf.cur, dlt);
        buf.sz += dlt;
      }
      buf.cur = buf.base;
      return HandleDownstream(buf, N, r);
    }

    bool
    TransitHop::HandleDownstream(llarp_buffer_t buf, const TunnelNonce& Y,
                                 llarp_router* r)
    {
      RelayDownstreamMessage msg;
      msg.pathid = info.rxID;
      msg.Y      = Y ^ nonceXOR;
      r->crypto.xchacha20(buf, pathKey, Y);
      msg.X = buf;
      llarp::LogDebug("relay ", msg.X.size(), " bytes downstream from ",
                      info.upstream, " to ", info.downstream);
      return r->SendToOrQueue(info.downstream, &msg);
    }

    bool
    TransitHop::HandleUpstream(llarp_buffer_t buf, const TunnelNonce& Y,
                               llarp_router* r)
    {
      r->crypto.xchacha20(buf, pathKey, Y);
      if(IsEndpoint(r->pubkey()))
      {
        return m_MessageParser.ParseMessageBuffer(buf, this, info.rxID, r);
      }
      else
      {
        RelayUpstreamMessage msg;
        msg.pathid = info.txID;
        msg.Y      = Y ^ nonceXOR;

        msg.X = buf;
        llarp::LogDebug("relay ", msg.X.size(), " bytes upstream from ",
                        info.downstream, " to ", info.upstream);
        return r->SendToOrQueue(info.upstream, &msg);
      }
    }

    bool
    TransitHop::HandleDHTMessage(const llarp::dht::IMessage* msg,
                                 llarp_router* r)
    {
      return r->dht->impl.RelayRequestForPath(info.rxID, msg);
    }

    bool
    TransitHop::HandlePathLatencyMessage(
        const llarp::routing::PathLatencyMessage* msg, llarp_router* r)
    {
      llarp::routing::PathLatencyMessage reply;
      reply.L = msg->T;
      return SendRoutingMessage(&reply, r);
    }

    bool
    TransitHop::HandlePathConfirmMessage(
        const llarp::routing::PathConfirmMessage* msg, llarp_router* r)
    {
      llarp::LogWarn("unwarrented path confirm message on ", info);
      return false;
    }

    bool
    TransitHop::HandleDataDiscardMessage(
        const llarp::routing::DataDiscardMessage* msg, llarp_router* r)
    {
      llarp::LogWarn("unwarranted path data discard message on ", info);
      return false;
    }

    bool
    TransitHop::HandlePathTransferMessage(
        const llarp::routing::PathTransferMessage* msg, llarp_router* r)
    {
      auto path = r->paths.GetPathForTransfer(msg->P);
      if(!path)
      {
        llarp::routing::DataDiscardMessage discarded(msg->P, msg->S);
        path = r->paths.GetPathForTransfer(msg->from);
        return path && path->SendRoutingMessage(&discarded, r);
      }

      byte_t tmp[service::MAX_PROTOCOL_MESSAGE_SIZE];
      auto buf = llarp::StackBuffer< decltype(tmp) >(tmp);
      if(!msg->T.BEncode(&buf))
      {
        llarp::LogWarn("failed to transfer data message, encode failed");
        return false;
      }
      // rewind0
      buf.sz  = buf.cur - buf.base;
      buf.cur = buf.base;
      // send
      return path->HandleDownstream(buf, msg->Y, r);
    }

  }  // namespace path
}  // namespace llarp
