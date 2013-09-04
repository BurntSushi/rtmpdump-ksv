/*  Simple RTMP Server
 *  Copyright (C) 2009 Andrej Stepanchuk
 *  Copyright (C) 2009-2011 Howard Chu
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RTMPDump; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

/* This is just a stub for an RTMP server. It doesn't do anything
 * beyond obtaining the connection parameters from the client.
 */

#include <stdlib.h>
#ifdef __MINGW_H
#include <unistd.h>
#endif
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#include <signal.h>
#include <getopt.h>

#include <assert.h>

#include "librtmp/rtmp_sys.h"
#include "librtmp/log.h"

#include "thread.h"

#ifdef linux
#include <linux/netfilter_ipv4.h>
#endif

#ifndef WIN32
#include <sys/types.h>
#include <sys/wait.h>
#endif

#define RD_SUCCESS		0
#define RD_FAILED		1
#define RD_INCOMPLETE		2

#define PACKET_SIZE 1024*1024

#ifdef WIN32
#define InitSockets()	{\
	WORD version;			\
	WSADATA wsaData;		\
					\
	version = MAKEWORD(1,1);	\
	WSAStartup(version, &wsaData);	}

#define	CleanupSockets()	WSACleanup()
#else
#define InitSockets()
#define	CleanupSockets()
#endif

#define DUPTIME	5000	/* interval we disallow duplicate requests, in msec */

enum
{
  STREAMING_ACCEPTING,
  STREAMING_IN_PROGRESS,
  STREAMING_STOPPING,
  STREAMING_STOPPED
};

typedef struct
{
  int socket;
  int state;
  int streamID;
  int arglen;
  int argc;
  uint32_t filetime;	/* time of last download we started */
  AVal filename;	/* name of last download */
  char *connect;

} STREAMING_SERVER;

STREAMING_SERVER *rtmpServer = 0;	// server structure pointer
void *sslCtx = NULL;

STREAMING_SERVER *startStreaming(const char *address, int port);
void stopStreaming(STREAMING_SERVER * server);
void AVreplace(AVal *src, const AVal *orig, const AVal *repl);
char *strreplace(char *srcstr, int srclen, char *orig, char *repl, int didAlloc);
int file_exists(const char *fname);
int SendCheckBWResponse(RTMP *r, int oldMethodType, int onBWDoneInit);
AVal AVcopy(AVal src);
AVal StripParams(AVal *src);

static const AVal av_dquote = AVC("\"");
static const AVal av_escdquote = AVC("\\\"");
#ifdef WIN32
static const AVal av_caret = AVC("^");
static const AVal av_esccaret = AVC("^^");
static const AVal av_pipe = AVC("|");
static const AVal av_escpipe = AVC("^|");
#endif

typedef struct
{
  char *hostname;
  int rtmpport;
  int protocol;
  int bLiveStream;		// is it a live stream? then we can't seek/resume

  long int timeout;		// timeout connection afte 300 seconds
  uint32_t bufferTime;

  char *rtmpurl;
  AVal playpath;
  AVal swfUrl;
  AVal tcUrl;
  AVal pageUrl;
  AVal app;
  AVal auth;
  AVal swfHash;
  AVal flashVer;
  AVal subscribepath;
  uint32_t swfSize;

  uint32_t dStartOffset;
  uint32_t dStopOffset;
  uint32_t nTimeStamp;
} RTMP_REQUEST;

#define STR2AVAL(av,str)	av.av_val = str; av.av_len = strlen(av.av_val)

/* this request is formed from the parameters and used to initialize a new request,
 * thus it is a default settings list. All settings can be overriden by specifying the
 * parameters in the GET request. */
RTMP_REQUEST defaultRTMPRequest;

#ifdef _DEBUG
uint32_t debugTS = 0;

int pnum = 0;

FILE *netstackdump = NULL;
FILE *netstackdump_read = NULL;
#endif

#define SAVC(x) static const AVal av_##x = AVC(#x)

SAVC(app);
SAVC(connect);
SAVC(flashVer);
SAVC(swfUrl);
SAVC(pageUrl);
SAVC(tcUrl);
SAVC(fpad);
SAVC(capabilities);
SAVC(audioCodecs);
SAVC(videoCodecs);
SAVC(videoFunction);
SAVC(objectEncoding);
SAVC(_result);
SAVC(createStream);
SAVC(getStreamLength);
SAVC(play);
SAVC(fmsVer);
SAVC(mode);
SAVC(level);
SAVC(code);
SAVC(description);
SAVC(secureToken);
SAVC(_checkbw);
SAVC(_onbwdone);
SAVC(checkBandwidth);
SAVC(onBWDone);
SAVC(FCSubscribe);
SAVC(onFCSubscribe);

static int
SendConnectResult(RTMP *r, double txn)
{
  RTMPPacket packet;
  char pbuf[384], *pend = pbuf+sizeof(pbuf);
  AMFObject obj;
  AMFObjectProperty p, op;
  AVal av;

  packet.m_nChannel = 0x03;     // control channel (invoke)
  packet.m_headerType = 1; /* RTMP_PACKET_SIZE_MEDIUM; */
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  char *enc = packet.m_body;
  enc = AMF_EncodeString(enc, pend, &av__result);
  enc = AMF_EncodeNumber(enc, pend, txn);
  *enc++ = AMF_OBJECT;

  STR2AVAL(av, "FMS/3,5,7,7009");
  enc = AMF_EncodeNamedString(enc, pend, &av_fmsVer, &av);
  enc = AMF_EncodeNamedNumber(enc, pend, &av_capabilities, 31.0);
  enc = AMF_EncodeNamedNumber(enc, pend, &av_mode, 1.0);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  *enc++ = AMF_OBJECT;

  STR2AVAL(av, "status");
  enc = AMF_EncodeNamedString(enc, pend, &av_level, &av);
  STR2AVAL(av, "NetConnection.Connect.Success");
  enc = AMF_EncodeNamedString(enc, pend, &av_code, &av);
  STR2AVAL(av, "Connection succeeded.");
  enc = AMF_EncodeNamedString(enc, pend, &av_description, &av);
  enc = AMF_EncodeNamedNumber(enc, pend, &av_objectEncoding, r->m_fEncoding);
#if 0
  STR2AVAL(av, "58656322c972d6cdf2d776167575045f8484ea888e31c086f7b5ffbd0baec55ce442c2fb");
  enc = AMF_EncodeNamedString(enc, pend, &av_secureToken, &av);
#endif
  STR2AVAL(p.p_name, "version");
  STR2AVAL(p.p_vu.p_aval, "3,5,7,7009");
  p.p_type = AMF_STRING;
  obj.o_num = 1;
  obj.o_props = &p;
  op.p_type = AMF_OBJECT;
  STR2AVAL(op.p_name, "data");
  op.p_vu.p_object = obj;
  enc = AMFProp_Encode(&op, enc, pend);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  packet.m_nBodySize = enc - packet.m_body;

  return RTMP_SendPacket(r, &packet, FALSE);
}

static int
SendResultNumber(RTMP *r, double txn, double ID)
{
  RTMPPacket packet;
  char pbuf[1024], *pend = pbuf + sizeof (pbuf);

  packet.m_nChannel = 0x03;     // control channel (invoke)
  packet.m_headerType = 1; /* RTMP_PACKET_SIZE_MEDIUM; */
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  char *enc = packet.m_body;
  enc = AMF_EncodeString(enc, pend, &av__result);
  enc = AMF_EncodeNumber(enc, pend, txn);
  *enc++ = AMF_NULL;
  enc = AMF_EncodeNumber(enc, pend, ID);

  packet.m_nBodySize = enc - packet.m_body;

  return RTMP_SendPacket(r, &packet, FALSE);
}

SAVC(onStatus);
SAVC(status);
static const AVal av_NetStream_Play_Start = AVC("NetStream.Play.Start");
static const AVal av_Started_playing = AVC("Started playing");
static const AVal av_NetStream_Play_Stop = AVC("NetStream.Play.Stop");
static const AVal av_Stopped_playing = AVC("Stopped playing");
SAVC(details);
SAVC(clientid);
static const AVal av_NetStream_Authenticate_UsherToken = AVC("NetStream.Authenticate.UsherToken");
static const AVal av_FCSubscribe_message = AVC("FCSubscribe to stream");

static int
SendPlayStart(RTMP *r)
{
  RTMPPacket packet;
  char pbuf[1024], *pend = pbuf + sizeof (pbuf);

  packet.m_nChannel = 0x03;     // control channel (invoke)
  packet.m_headerType = 1; /* RTMP_PACKET_SIZE_MEDIUM; */
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  char *enc = packet.m_body;
  enc = AMF_EncodeString(enc, pend, &av_onStatus);
  enc = AMF_EncodeNumber(enc, pend, 0);
  *enc++ = AMF_OBJECT;

  enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_status);
  enc = AMF_EncodeNamedString(enc, pend, &av_code, &av_NetStream_Play_Start);
  enc = AMF_EncodeNamedString(enc, pend, &av_description, &av_Started_playing);
  enc = AMF_EncodeNamedString(enc, pend, &av_details, &r->Link.playpath);
  enc = AMF_EncodeNamedString(enc, pend, &av_clientid, &av_clientid);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  packet.m_nBodySize = enc - packet.m_body;
  return RTMP_SendPacket(r, &packet, FALSE);
}

static int
SendPlayStop(RTMP *r)
{
  RTMPPacket packet;
  char pbuf[1024], *pend = pbuf + sizeof (pbuf);

  packet.m_nChannel = 0x03;     // control channel (invoke)
  packet.m_headerType = 1; /* RTMP_PACKET_SIZE_MEDIUM; */
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  char *enc = packet.m_body;
  enc = AMF_EncodeString(enc, pend, &av_onStatus);
  enc = AMF_EncodeNumber(enc, pend, 0);
  *enc++ = AMF_OBJECT;

  enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_status);
  enc = AMF_EncodeNamedString(enc, pend, &av_code, &av_NetStream_Play_Stop);
  enc = AMF_EncodeNamedString(enc, pend, &av_description, &av_Stopped_playing);
  enc = AMF_EncodeNamedString(enc, pend, &av_details, &r->Link.playpath);
  enc = AMF_EncodeNamedString(enc, pend, &av_clientid, &av_clientid);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  packet.m_nBodySize = enc - packet.m_body;
  return RTMP_SendPacket(r, &packet, FALSE);
}

int
SendCheckBWResponse(RTMP *r, int oldMethodType, int onBWDoneInit)
{
  RTMPPacket packet;
  char pbuf[1024], *pend = pbuf + sizeof (pbuf);
  char *enc;

  packet.m_nChannel = 0x03; /* control channel (invoke) */
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  enc = packet.m_body;
  if (oldMethodType)
    {
      enc = AMF_EncodeString(enc, pend, &av__onbwdone);
      enc = AMF_EncodeNumber(enc, pend, 0);
      *enc++ = AMF_NULL;
      enc = AMF_EncodeNumber(enc, pend, 10240);
      enc = AMF_EncodeNumber(enc, pend, 0);
    }
  else
    {
      enc = AMF_EncodeString(enc, pend, &av_onBWDone);
      enc = AMF_EncodeNumber(enc, pend, 0);
      *enc++ = AMF_NULL;
      if (!onBWDoneInit)
        {
          enc = AMF_EncodeNumber(enc, pend, 10240);
          enc = AMF_EncodeNumber(enc, pend, 0);
          enc = AMF_EncodeNumber(enc, pend, 0);
          enc = AMF_EncodeNumber(enc, pend, 20);
        }
    }

  packet.m_nBodySize = enc - packet.m_body;

  return RTMP_SendPacket(r, &packet, FALSE);
}

int
SendOnFCSubscribe(RTMP *r)
{
  RTMPPacket packet;
  char pbuf[1024], *pend = pbuf + sizeof (pbuf);
  char *enc;

  packet.m_nChannel = 0x03; /* control channel (invoke) */
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
  packet.m_nTimeStamp = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;
  packet.m_body = pbuf + RTMP_MAX_HEADER_SIZE;

  enc = packet.m_body;
  enc = AMF_EncodeString(enc, pend, &av_onFCSubscribe);
  enc = AMF_EncodeNumber(enc, pend, 0);
  *enc++ = AMF_NULL;

  *enc++ = AMF_OBJECT;
  enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_status);
  enc = AMF_EncodeNamedString(enc, pend, &av_code, &av_NetStream_Play_Start);
  enc = AMF_EncodeNamedString(enc, pend, &av_description, &av_FCSubscribe_message);
  enc = AMF_EncodeNamedNumber(enc, pend, &av_clientid, 0);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  packet.m_nBodySize = enc - packet.m_body;

  return RTMP_SendPacket(r, &packet, FALSE);
}

static void
spawn_dumper(int argc, AVal *av, char *cmd)
{
#ifdef WIN32
  STARTUPINFO si = {0};
  PROCESS_INFORMATION pi = {0};

  si.cb = sizeof(si);
  if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
    &si, &pi))
    {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
#else
  /* reap any dead children */
  while (waitpid(-1, NULL, WNOHANG) > 0);

  if (fork() == 0) {
    char **argv = malloc((argc+1) * sizeof(char *));
    int i;

    for (i=0; i<argc; i++) {
      argv[i] = av[i].av_val;
      argv[i][av[i].av_len] = '\0';
    }
    argv[i] = NULL;
    if ((i = execvp(argv[0], argv)))
      _exit(i);
  }
#endif
}

static int
countAMF(AMFObject *obj, int *argc)
{
  int i, len;

  for (i=0, len=0; i < obj->o_num; i++)
    {
      AMFObjectProperty *p = &obj->o_props[i];
      len += 4;
      (*argc)+= 2;
      if (p->p_name.av_val)
	len += 1;
      len += 2;
      if (p->p_name.av_val)
	len += p->p_name.av_len + 1;
      switch(p->p_type)
	{
	case AMF_BOOLEAN:
	  len += 1;
	  break;
	case AMF_STRING:
	  len += p->p_vu.p_aval.av_len;
	  break;
	case AMF_NUMBER:
	  len += 40;
	  break;
	case AMF_OBJECT:
        case AMF_ECMA_ARRAY:
        case AMF_STRICT_ARRAY:
	  len += 9;
	  len += countAMF(&p->p_vu.p_object, argc);
	  (*argc) += 2;
	  break;
	case AMF_NULL:
	default:
	  break;
	}
    }
  return len;
}

static char *
dumpAMF(AMFObject *obj, char *ptr, AVal *argv, int *argc)
{
  int i, ac = *argc;
  const char opt[] = "NBSO Z";

  for (i = 0; i < obj->o_num; i++)
    {
      AMFObjectProperty *p = &obj->o_props[i];
      if ((p->p_type == AMF_ECMA_ARRAY) || (p->p_type == AMF_STRICT_ARRAY))
        p->p_type = AMF_OBJECT;
      argv[ac].av_val = ptr+1;
      argv[ac++].av_len = 2;
      ptr += sprintf(ptr, " -C ");
      argv[ac].av_val = ptr;
      if (p->p_name.av_val)
	*ptr++ = 'N';
      *ptr++ = opt[p->p_type];
      *ptr++ = ':';
      if (p->p_name.av_val)
	ptr += sprintf(ptr, "%.*s:", p->p_name.av_len, p->p_name.av_val);
      switch(p->p_type)
	{
	case AMF_BOOLEAN:
	  *ptr++ = p->p_vu.p_number != 0 ? '1' : '0';
	  argv[ac].av_len = ptr - argv[ac].av_val;
	  break;
	case AMF_STRING:
	  memcpy(ptr, p->p_vu.p_aval.av_val, p->p_vu.p_aval.av_len);
	  ptr += p->p_vu.p_aval.av_len;
	  argv[ac].av_len = ptr - argv[ac].av_val;
	  break;
	case AMF_NUMBER:
	  ptr += sprintf(ptr, "%f", p->p_vu.p_number);
	  argv[ac].av_len = ptr - argv[ac].av_val;
	  break;
	case AMF_OBJECT:
	  *ptr++ = '1';
	  argv[ac].av_len = ptr - argv[ac].av_val;
	  ac++;
	  *argc = ac;
	  ptr = dumpAMF(&p->p_vu.p_object, ptr, argv, argc);
	  ac = *argc;
	  argv[ac].av_val = ptr+1;
	  argv[ac++].av_len = 2;
	  argv[ac].av_val = ptr+4;
	  argv[ac].av_len = 3;
	  ptr += sprintf(ptr, " -C O:0");
	  break;
	case AMF_NULL:
	default:
	  argv[ac].av_len = ptr - argv[ac].av_val;
	  break;
	}
      ac++;
    }
  *argc = ac;
  return ptr;
}

// Returns 0 for OK/Failed/error, 1 for 'Stop or Complete'
int
ServeInvoke(STREAMING_SERVER *server, RTMP * r, RTMPPacket *packet, unsigned int offset)
{
  const char *body;
  unsigned int nBodySize;
  int ret = 0, nRes;

  body = packet->m_body + offset;
  nBodySize = packet->m_nBodySize - offset;

  if (body[0] != 0x02)		// make sure it is a string method name we start with
    {
      RTMP_Log(RTMP_LOGWARNING, "%s, Sanity failed. no string method in invoke packet",
	  __FUNCTION__);
      return 0;
    }

  AMFObject obj;
  nRes = AMF_Decode(&obj, body, nBodySize, FALSE);
  if (nRes < 0)
    {
      RTMP_Log(RTMP_LOGERROR, "%s, error decoding invoke packet", __FUNCTION__);
      return 0;
    }

  AMF_Dump(&obj);
  AVal method;
  AMFProp_GetString(AMF_GetProp(&obj, NULL, 0), &method);
  double txn = AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 1));
  RTMP_Log(RTMP_LOGDEBUG, "%s, client invoking <%s>", __FUNCTION__, method.av_val);

  if (AVMATCH(&method, &av_connect))
    {
      AMFObject cobj;
      AVal pname, pval;
      int i;

      server->connect = packet->m_body;
      packet->m_body = NULL;

      AMFProp_GetObject(AMF_GetProp(&obj, NULL, 2), &cobj);
      for (i=0; i<cobj.o_num; i++)
	{
	  pname = cobj.o_props[i].p_name;
	  pval.av_val = NULL;
	  pval.av_len = 0;
	  if (cobj.o_props[i].p_type == AMF_STRING)
	    pval = cobj.o_props[i].p_vu.p_aval;
	  if (AVMATCH(&pname, &av_app))
	    {
	      r->Link.app = pval;
	      pval.av_val = NULL;
	      if (!r->Link.app.av_val)
	        r->Link.app.av_val = "";
	      server->arglen += 6 + pval.av_len;
	      server->argc += 2;
	    }
	  else if (AVMATCH(&pname, &av_flashVer))
	    {
	      r->Link.flashVer = pval;
	      pval.av_val = NULL;
	      server->arglen += 6 + pval.av_len;
	      server->argc += 2;
	    }
	  else if (AVMATCH(&pname, &av_swfUrl))
	    {
	      r->Link.swfUrl = pval;
	      pval.av_val = NULL;
	      server->arglen += 6 + pval.av_len;
	      server->argc += 2;
	    }
	  else if (AVMATCH(&pname, &av_tcUrl))
	    {
	      r->Link.tcUrl = pval;
	      pval.av_val = NULL;
	      server->arglen += 6 + pval.av_len;
	      server->argc += 2;
	    }
	  else if (AVMATCH(&pname, &av_pageUrl))
	    {
	      r->Link.pageUrl = pval;
	      pval.av_val = NULL;
	      server->arglen += 6 + pval.av_len;
	      server->argc += 2;
	    }
	  else if (AVMATCH(&pname, &av_audioCodecs))
	    {
	      r->m_fAudioCodecs = cobj.o_props[i].p_vu.p_number;
	    }
	  else if (AVMATCH(&pname, &av_videoCodecs))
	    {
	      r->m_fVideoCodecs = cobj.o_props[i].p_vu.p_number;
	    }
	  else if (AVMATCH(&pname, &av_objectEncoding))
	    {
	      r->m_fEncoding = cobj.o_props[i].p_vu.p_number;
	    }
	}
      /* Still have more parameters? Copy them */
      if (obj.o_num > 3)
	{
	  int i = obj.o_num - 3;
	  r->Link.extras.o_num = i;
	  r->Link.extras.o_props = malloc(i*sizeof(AMFObjectProperty));
	  memcpy(r->Link.extras.o_props, obj.o_props+3, i*sizeof(AMFObjectProperty));
	  obj.o_num = 3;
	  server->arglen += countAMF(&r->Link.extras, &server->argc);
	}
      SendConnectResult(r, txn);
      SendCheckBWResponse(r, FALSE, TRUE);
    }
  else if (AVMATCH(&method, &av_createStream))
    {
      SendResultNumber(r, txn, ++server->streamID);
    }
  else if (AVMATCH(&method, &av_getStreamLength))
    {
      SendResultNumber(r, txn, 10.0);
    }
  else if (AVMATCH(&method, &av_NetStream_Authenticate_UsherToken))
    {
      AVal usherToken;
      AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &usherToken);
      AVreplace(&usherToken, &av_dquote, &av_escdquote);
#ifdef WIN32
      AVreplace(&usherToken, &av_caret, &av_esccaret);
      AVreplace(&usherToken, &av_pipe, &av_escpipe);
#endif
      server->arglen += 6 + usherToken.av_len;
      server->argc += 2;
      r->Link.usherToken = usherToken;
    }
  else if (AVMATCH(&method, &av__checkbw))
    {
      SendCheckBWResponse(r, TRUE, FALSE);
    }
  else if (AVMATCH(&method, &av_checkBandwidth))
    {
      SendCheckBWResponse(r, FALSE, FALSE);
    }
  else if (AVMATCH(&method, &av_FCSubscribe))
    {
      SendOnFCSubscribe(r);
    }
  else if (AVMATCH(&method, &av_play))
    {
      char *file, *p, *q, *cmd, *ptr;
      AVal *argv, av;
      int len, argc;
      uint32_t now;
      RTMPPacket pc = {0};
      AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &r->Link.playpath);
      /*
      r->Link.seekTime = AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 4));
      if (obj.o_num > 5)
	r->Link.length = AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 5));
      */
      double StartFlag = 0;
      AMFObjectProperty *Start = AMF_GetProp(&obj, NULL, 4);
      if (!(Start->p_type == AMF_INVALID))
        StartFlag = AMFProp_GetNumber(Start);
      r->Link.app = AVcopy(r->Link.app);
      if (StartFlag == -1000 || (r->Link.app.av_val && strstr(r->Link.app.av_val, "live")))
        {
          StartFlag = -1000;
          server->arglen += 7;
          server->argc += 1;
        }
      if (r->Link.tcUrl.av_len)
	{
	  len = server->arglen + r->Link.playpath.av_len + 4 +
	    sizeof("rtmpdump") + r->Link.playpath.av_len + 12;
	  server->argc += 5;

	  cmd = malloc(len + server->argc * sizeof(AVal));
	  ptr = cmd;
	  argv = (AVal *)(cmd + len);
	  argv[0].av_val = cmd;
	  argv[0].av_len = sizeof("rtmpdump")-1;
	  ptr += sprintf(ptr, "rtmpdump");
	  argc = 1;

	  argv[argc].av_val = ptr + 1;
	  argv[argc++].av_len = 2;
	  argv[argc].av_val = ptr + 5;
	  r->Link.tcUrl = StripParams(&r->Link.tcUrl);
	  ptr += sprintf(ptr," -r \"%s\"", r->Link.tcUrl.av_val);
	  argv[argc++].av_len = r->Link.tcUrl.av_len;

	  if (r->Link.app.av_val)
	    {
	      argv[argc].av_val = ptr + 1;
	      argv[argc++].av_len = 2;
	      argv[argc].av_val = ptr + 5;
	      ptr += sprintf(ptr, " -a \"%s\"", r->Link.app.av_val);
	      argv[argc++].av_len = r->Link.app.av_len;
	    }
	  if (r->Link.flashVer.av_val)
	    {
	      argv[argc].av_val = ptr + 1;
	      argv[argc++].av_len = 2;
	      argv[argc].av_val = ptr + 5;
	      ptr += sprintf(ptr, " -f \"%s\"", r->Link.flashVer.av_val);
	      argv[argc++].av_len = r->Link.flashVer.av_len;
	    }
	  if (r->Link.swfUrl.av_val)
	    {
	      argv[argc].av_val = ptr + 1;
	      argv[argc++].av_len = 2;
	      argv[argc].av_val = ptr + 5;
	      r->Link.swfUrl = StripParams(&r->Link.swfUrl);
	      ptr += sprintf(ptr, " -W \"%s\"", r->Link.swfUrl.av_val);
	      argv[argc++].av_len = r->Link.swfUrl.av_len;
	    }
	  if (r->Link.pageUrl.av_val)
	    {
	      argv[argc].av_val = ptr + 1;
	      argv[argc++].av_len = 2;
	      argv[argc].av_val = ptr + 5;
	      ptr += sprintf(ptr, " -p \"%s\"", r->Link.pageUrl.av_val);
	      argv[argc++].av_len = r->Link.pageUrl.av_len;
	    }
	  if (r->Link.usherToken.av_val)
	    {
	      argv[argc].av_val = ptr + 1;
	      argv[argc++].av_len = 2;
	      argv[argc].av_val = ptr + 5;
	      ptr += sprintf(ptr, " -j \"%s\"", r->Link.usherToken.av_val);
	      argv[argc++].av_len = r->Link.usherToken.av_len;
	      free(r->Link.usherToken.av_val);
	      r->Link.usherToken.av_val = NULL;
	      r->Link.usherToken.av_len = 0;
	    }
          if (StartFlag == -1000)
            {
              argv[argc].av_val = ptr + 1;
              argv[argc++].av_len = 6;
              ptr += sprintf(ptr, " --live");
            }
          if (r->Link.extras.o_num)
            {
              ptr = dumpAMF(&r->Link.extras, ptr, argv, &argc);
              AMF_Reset(&r->Link.extras);
            }
	  argv[argc].av_val = ptr + 1;
	  argv[argc++].av_len = 2;
	  argv[argc].av_val = ptr + 5;
	  ptr += sprintf(ptr, " -y \"%.*s\"",
	    r->Link.playpath.av_len, r->Link.playpath.av_val);
	  argv[argc++].av_len = r->Link.playpath.av_len;

          if (r->Link.playpath.av_len)
            av = r->Link.playpath;
          else
            {
              av.av_val = "file";
              av.av_len = 4;
            }
	  /* strip trailing URL parameters */
	  q = memchr(av.av_val, '?', av.av_len);
	  if (q)
	    {
	      if (q == av.av_val)
		{
		  av.av_val++;
		  av.av_len--;
		}
	      else
		{
		  av.av_len = q - av.av_val;
		}
	    }
	  /* strip leading slash components */
	  for (p=av.av_val+av.av_len-1; p>=av.av_val; p--)
	    if (*p == '/')
	      {
		p++;
		av.av_len -= p - av.av_val;
		av.av_val = p;
		break;
	      }
	  /* skip leading dot */
	  if (av.av_val[0] == '.')
	    {
	      av.av_val++;
	      av.av_len--;
	    }
	  file = malloc(av.av_len+5);

	  memcpy(file, av.av_val, av.av_len);
	  file[av.av_len] = '\0';

          if (strlen(file) < 128)
            {
              /* Add extension if none present */
              if (file[av.av_len - 4] != '.')
                {
                  av.av_len += 4;
                }

              /* Always use flv extension, regardless of original */
              if (strcmp(file + av.av_len - 4, ".flv"))
                {
                  strcpy(file + av.av_len - 4, ".flv");
                }

              /* Remove invalid characters from filename */
              file = strreplace(file, 0, ":", "_", TRUE);
              file = strreplace(file, 0, "&", "_", TRUE);
              file = strreplace(file, 0, "^", "_", TRUE);
              file = strreplace(file, 0, "|", "_", TRUE);
            }
          else
            {
              /* Filename too long - generate unique name */
              strcpy(file, "vXXXXXX");
              mktemp(file);
              strcat(file, ".flv");
            }

          /* Add timestamp to the filename */
          char *filename, *pfilename, timestamp[21];
          int filename_len, timestamp_len;
          time_t current_time;

          time(&current_time);
          timestamp_len = strftime(&timestamp[0], sizeof (timestamp), "%Y-%m-%d_%I-%M-%S_", localtime(&current_time));
          timestamp[timestamp_len] = '\0';
          filename_len = strlen(file);
          filename = malloc(timestamp_len + filename_len + 1);
          pfilename = filename;
          memcpy(pfilename, timestamp, timestamp_len);
          pfilename += timestamp_len;
          memcpy(pfilename, file, filename_len);
          pfilename += filename_len;
          *pfilename++ = '\0';
          file = filename;

	  argv[argc].av_val = ptr + 1;
	  argv[argc++].av_len = 2;
	  argv[argc].av_val = file;
	  argv[argc].av_len = av.av_len;
#ifdef VLC
          char *vlc;
          int didAlloc = FALSE;

          if (getenv("VLC"))
            vlc = getenv("VLC");
          else if (getenv("ProgramFiles"))
            {
              vlc = malloc(512 * sizeof (char));
              didAlloc = TRUE;
              char *ProgramFiles = getenv("ProgramFiles");
              sprintf(vlc, "\"%s%s", ProgramFiles, " (x86)\\VideoLAN\\VLC\\vlc.exe");
              if (!file_exists(vlc + 1))
                sprintf(vlc + 1, "%s%s", ProgramFiles, "\\VideoLAN\\VLC\\vlc.exe");
              strcpy(vlc + strlen(vlc), "\" -");
            }
          else
            vlc = "vlc -";

          ptr += sprintf(ptr, " | %s", vlc);
          if (didAlloc)
            free(vlc);
#else
          ptr += sprintf(ptr, " -o \"%s\"", file);
#endif
	  now = RTMP_GetTime();
	  if (now - server->filetime < DUPTIME && AVMATCH(&argv[argc], &server->filename))
	    {
	      printf("Duplicate request, skipping.\n");
	      free(file);
	    }
	  else
	    {
	      printf("\n%s\n\n", cmd);
	      fflush(stdout);
	      server->filetime = now;
	      free(server->filename.av_val);
	      server->filename = argv[argc++];
#ifdef VLC
              FILE *vlc_cmdfile = fopen("VLC.bat", "w");
              char *vlc_batchcmd = strreplace(cmd, 0, "%", "%%", FALSE);
              fprintf(vlc_cmdfile, "%s\n", vlc_batchcmd);
              fclose(vlc_cmdfile);
              free(vlc_batchcmd);
              spawn_dumper(argc, argv, "VLC.bat");
#else
              spawn_dumper(argc, argv, cmd);
#endif

              /* Save command to text file */
              FILE *cmdfile = fopen("Command.txt", "a");
              fprintf(cmdfile, "%s\n", cmd);
              fclose(cmdfile);
	    }

	  free(cmd);
	}
      pc.m_body = server->connect;
      server->connect = NULL;
      RTMPPacket_Free(&pc);
      ret = 1;
	  RTMP_SendCtrl(r, 0, 1, 0);
	  SendPlayStart(r);
	  RTMP_SendCtrl(r, 1, 1, 0);
	  SendPlayStop(r);
    }
  AMF_Reset(&obj);
  return ret;
}

int
ServePacket(STREAMING_SERVER *server, RTMP *r, RTMPPacket *packet)
{
  int ret = 0;

  RTMP_Log(RTMP_LOGDEBUG, "%s, received packet type %02X, size %u bytes", __FUNCTION__,
    packet->m_packetType, packet->m_nBodySize);

  switch (packet->m_packetType)
    {
    case RTMP_PACKET_TYPE_CHUNK_SIZE:
//      HandleChangeChunkSize(r, packet);
      break;

    case RTMP_PACKET_TYPE_BYTES_READ_REPORT:
      break;

    case RTMP_PACKET_TYPE_CONTROL:
//      HandleCtrl(r, packet);
      break;

    case RTMP_PACKET_TYPE_SERVER_BW:
//      HandleServerBW(r, packet);
      break;

    case RTMP_PACKET_TYPE_CLIENT_BW:
 //     HandleClientBW(r, packet);
      break;

    case RTMP_PACKET_TYPE_AUDIO:
      //RTMP_Log(RTMP_LOGDEBUG, "%s, received: audio %lu bytes", __FUNCTION__, packet.m_nBodySize);
      break;

    case RTMP_PACKET_TYPE_VIDEO:
      //RTMP_Log(RTMP_LOGDEBUG, "%s, received: video %lu bytes", __FUNCTION__, packet.m_nBodySize);
      break;

    case RTMP_PACKET_TYPE_FLEX_STREAM_SEND:
      break;

    case RTMP_PACKET_TYPE_FLEX_SHARED_OBJECT:
      break;

    case RTMP_PACKET_TYPE_FLEX_MESSAGE:
      {
	RTMP_Log(RTMP_LOGDEBUG, "%s, flex message, size %u bytes, not fully supported",
	    __FUNCTION__, packet->m_nBodySize);
	//RTMP_LogHex(packet.m_body, packet.m_nBodySize);

	// some DEBUG code
	/*RTMP_LIB_AMFObject obj;
	   int nRes = obj.Decode(packet.m_body+1, packet.m_nBodySize-1);
	   if(nRes < 0) {
	   RTMP_Log(RTMP_LOGERROR, "%s, error decoding AMF3 packet", __FUNCTION__);
	   //return;
	   }

	   obj.Dump(); */

	if (ServeInvoke(server, r, packet, 1))
	  RTMP_Close(r);
	break;
      }
    case RTMP_PACKET_TYPE_INFO:
      break;

    case RTMP_PACKET_TYPE_SHARED_OBJECT:
      break;

    case RTMP_PACKET_TYPE_INVOKE:
      RTMP_Log(RTMP_LOGDEBUG, "%s, received: invoke %u bytes", __FUNCTION__,
	  packet->m_nBodySize);
      //RTMP_LogHex(packet.m_body, packet.m_nBodySize);

      if (ServeInvoke(server, r, packet, 0))
	RTMP_Close(r);
      break;

    case RTMP_PACKET_TYPE_FLASH_VIDEO:
	break;
    default:
      RTMP_Log(RTMP_LOGDEBUG, "%s, unknown packet type received: 0x%02x", __FUNCTION__,
	  packet->m_packetType);
#ifdef _DEBUG
      RTMP_LogHex(RTMP_LOGDEBUG, packet->m_body, packet->m_nBodySize);
#endif
    }
  return ret;
}

TFTYPE
controlServerThread(void *unused)
{
  char ich;
  while (1)
    {
      ich = getchar();
      switch (ich)
	{
	case 'q':
	  RTMP_LogPrintf("Exiting\n");
          if (rtmpServer)
            stopStreaming(rtmpServer);
	  break;
	default:
	  RTMP_LogPrintf("Unknown command \'%c\', ignoring\n", ich);
	}
      sleep(1);
      if (rtmpServer && (rtmpServer->state == STREAMING_STOPPED))
        {
          RTMP_Log(RTMP_LOGDEBUG, "Exiting text UI thread");
          break;
        }
    }
  TFRET();
}


void doServe(STREAMING_SERVER * server,	// server socket and state (our listening socket)
  int sockfd	// client connection socket
  )
{
  server->state = STREAMING_IN_PROGRESS;

  RTMP *rtmp = RTMP_Alloc();		/* our session with the real client */
  RTMPPacket packet = { 0 };

  // timeout for http requests
  fd_set fds;
  struct timeval tv;

  memset(&tv, 0, sizeof(struct timeval));
  tv.tv_sec = 5;

  FD_ZERO(&fds);
  FD_SET(sockfd, &fds);

  if (select(sockfd + 1, &fds, NULL, NULL, &tv) <= 0)
    {
      RTMP_Log(RTMP_LOGERROR, "Request timeout/select failed, ignoring request");
      goto quit;
    }
  else
    {
      RTMP_Init(rtmp);
      rtmp->m_sb.sb_socket = sockfd;
      if (sslCtx && !RTMP_TLS_Accept(rtmp, sslCtx))
        {
	  RTMP_Log(RTMP_LOGERROR, "TLS handshake failed");
	  goto cleanup;
        }
      if (!RTMP_Serve(rtmp))
	{
	  RTMP_Log(RTMP_LOGERROR, "Handshake failed");
	  goto cleanup;
	}
    }
  server->arglen = 0;
  while (RTMP_IsConnected(rtmp) && RTMP_ReadPacket(rtmp, &packet))
    {
      if (!RTMPPacket_IsReady(&packet))
	continue;
      ServePacket(server, rtmp, &packet);
      RTMPPacket_Free(&packet);
    }

cleanup:
  RTMP_LogPrintf("Closing connection... ");
  RTMP_Close(rtmp);
  /* Should probably be done by RTMP_Close() ... */
  rtmp->Link.playpath.av_val = NULL;
  rtmp->Link.tcUrl.av_val = NULL;
  rtmp->Link.swfUrl.av_val = NULL;
  rtmp->Link.pageUrl.av_val = NULL;
  rtmp->Link.app.av_val = NULL;
  rtmp->Link.flashVer.av_val = NULL;
  if (rtmp->Link.usherToken.av_val)
    {
      free(rtmp->Link.usherToken.av_val);
      rtmp->Link.usherToken.av_val = NULL;
    }
  RTMP_Free(rtmp);
  RTMP_LogPrintf("done!\n\n");

quit:
  if (server->state == STREAMING_IN_PROGRESS)
    server->state = STREAMING_ACCEPTING;

  return;
}

TFTYPE
serverThread(void *arg)
{
  STREAMING_SERVER *server = arg;
  server->state = STREAMING_ACCEPTING;

  while (server->state == STREAMING_ACCEPTING)
    {
      struct sockaddr_in addr;
      socklen_t addrlen = sizeof(struct sockaddr_in);
      int sockfd =
	accept(server->socket, (struct sockaddr *) &addr, &addrlen);

      if (sockfd > 0)
	{
#ifdef linux
	  struct sockaddr_in dest;
	  char destch[16];
	  socklen_t destlen = sizeof(struct sockaddr_in);
	  getsockopt(sockfd, SOL_IP, SO_ORIGINAL_DST, &dest, &destlen);
	  strcpy(destch, inet_ntoa(dest.sin_addr));
	  RTMP_Log(RTMP_LOGDEBUG, "%s: accepted connection from %s to %s\n", __FUNCTION__,
	      inet_ntoa(addr.sin_addr), destch);
#else
	  RTMP_Log(RTMP_LOGDEBUG, "%s: accepted connection from %s\n", __FUNCTION__,
	      inet_ntoa(addr.sin_addr));
#endif
	  /* Create a new thread and transfer the control to that */
	  doServe(server, sockfd);
	  RTMP_Log(RTMP_LOGDEBUG, "%s: processed request\n", __FUNCTION__);
	}
      else
	{
	  RTMP_Log(RTMP_LOGERROR, "%s: accept failed", __FUNCTION__);
	}
    }
  server->state = STREAMING_STOPPED;
  TFRET();
}

STREAMING_SERVER *
startStreaming(const char *address, int port)
{
  struct sockaddr_in addr;
  int sockfd, tmp;
  STREAMING_SERVER *server;

  sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sockfd == -1)
    {
      RTMP_Log(RTMP_LOGERROR, "%s, couldn't create socket", __FUNCTION__);
      return 0;
    }

  tmp = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
				(char *) &tmp, sizeof(tmp) );

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(address);	//htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) ==
      -1)
    {
      RTMP_Log(RTMP_LOGERROR, "%s, TCP bind failed for port number: %d", __FUNCTION__,
	  port);
      return 0;
    }

  if (listen(sockfd, 10) == -1)
    {
      RTMP_Log(RTMP_LOGERROR, "%s, listen failed", __FUNCTION__);
      closesocket(sockfd);
      return 0;
    }

  server = (STREAMING_SERVER *) calloc(1, sizeof(STREAMING_SERVER));
  server->socket = sockfd;

  ThreadCreate(serverThread, server);

  return server;
}

void
stopStreaming(STREAMING_SERVER * server)
{
  assert(server);

  if (server->state != STREAMING_STOPPED)
    {
      if (server->state == STREAMING_IN_PROGRESS)
	{
	  server->state = STREAMING_STOPPING;

	  // wait for streaming threads to exit
	  while (server->state != STREAMING_STOPPED)
	    msleep(1);
	}

      if (closesocket(server->socket))
	RTMP_Log(RTMP_LOGERROR, "%s: Failed to close listening socket, error %d",
	    __FUNCTION__, GetSockError());

      server->state = STREAMING_STOPPED;
    }
}

void
sigIntHandler(int sig)
{
  RTMP_ctrlC = TRUE;
  RTMP_LogPrintf("Caught signal: %d, cleaning up, just a second...\n", sig);
  if (rtmpServer)
    stopStreaming(rtmpServer);
  signal(SIGINT, SIG_DFL);
}

int
main(int argc, char **argv)
{
  int nStatus = RD_SUCCESS;
  int i;

  // http streaming server
  char DEFAULT_HTTP_STREAMING_DEVICE[] = "0.0.0.0";	// 0.0.0.0 is any device

  char *rtmpStreamingDevice = DEFAULT_HTTP_STREAMING_DEVICE;	// streaming device, default 0.0.0.0
  int nRtmpStreamingPort = 1935;	// port
  char *cert = NULL, *key = NULL;

  RTMP_LogPrintf("RTMP Server %s\n", RTMPDUMP_VERSION);
  RTMP_LogPrintf("(c) 2010 Andrej Stepanchuk, Howard Chu; license: GPL\n\n");

  RTMP_debuglevel = RTMP_LOGINFO;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp(argv[i], "-z"))
        RTMP_debuglevel = RTMP_LOGALL;
      else if (!strcmp(argv[i], "-c") && i + 1 < argc)
        cert = argv[++i];
      else if (!strcmp(argv[i], "-k") && i + 1 < argc)
        key = argv[++i];
    }

  if (cert && key)
    sslCtx = RTMP_TLS_AllocServerContext(cert, key);

  // init request
  memset(&defaultRTMPRequest, 0, sizeof(RTMP_REQUEST));

  defaultRTMPRequest.rtmpport = -1;
  defaultRTMPRequest.protocol = RTMP_PROTOCOL_UNDEFINED;
  defaultRTMPRequest.bLiveStream = FALSE;	// is it a live stream? then we can't seek/resume

  defaultRTMPRequest.timeout = 300;	// timeout connection afte 300 seconds
  defaultRTMPRequest.bufferTime = 20 * 1000;


  signal(SIGINT, sigIntHandler);
#ifndef WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _DEBUG
  netstackdump = fopen("netstackdump", "wb");
  netstackdump_read = fopen("netstackdump_read", "wb");
#endif

  InitSockets();

  // start text UI
  ThreadCreate(controlServerThread, 0);

  // start http streaming
  if ((rtmpServer =
       startStreaming(rtmpStreamingDevice, nRtmpStreamingPort)) == 0)
    {
      RTMP_Log(RTMP_LOGERROR, "Failed to start RTMP server, exiting!");
      return RD_FAILED;
    }
  RTMP_LogPrintf("Streaming on rtmp://%s:%d\n", rtmpStreamingDevice,
	    nRtmpStreamingPort);

  while (rtmpServer->state != STREAMING_STOPPED)
    {
      sleep(1);
    }
  RTMP_Log(RTMP_LOGDEBUG, "Done, exiting...");

  if (sslCtx)
    RTMP_TLS_FreeServerContext(sslCtx);

  CleanupSockets();

#ifdef _DEBUG
  if (netstackdump != 0)
    fclose(netstackdump);
  if (netstackdump_read != 0)
    fclose(netstackdump_read);
#endif
  return nStatus;
}

void
AVreplace(AVal *src, const AVal *orig, const AVal *repl)
{
  char *srcbeg = src->av_val;
  char *srcend = src->av_val + src->av_len;
  char *dest, *sptr, *dptr;
  int n = 0;

  /* count occurrences of orig in src */
  sptr = src->av_val;
  while (sptr < srcend && (sptr = strstr(sptr, orig->av_val)))
    {
      n++;
      sptr += orig->av_len;
    }
  if (!n)
    return;

  dest = malloc(src->av_len + 1 + (repl->av_len - orig->av_len) * n);

  sptr = src->av_val;
  dptr = dest;
  while (sptr < srcend && (sptr = strstr(sptr, orig->av_val)))
    {
      n = sptr - srcbeg;
      memcpy(dptr, srcbeg, n);
      dptr += n;
      memcpy(dptr, repl->av_val, repl->av_len);
      dptr += repl->av_len;
      sptr += orig->av_len;
      srcbeg = sptr;
    }
  n = srcend - srcbeg;
  memcpy(dptr, srcbeg, n);
  dptr += n;
  *dptr = '\0';
  src->av_val = dest;
  src->av_len = dptr - dest;
}

char *
strreplace(char *srcstr, int srclen, char *orig, char *repl, int didAlloc)
{
  char *ptr = NULL, *sptr = srcstr;
  int origlen = strlen(orig);
  int repllen = strlen(repl);
  if (!srclen)
    srclen = strlen(srcstr);
  char *srcend = srcstr + srclen;
  int dstbuffer = srclen / origlen * repllen;
  if (dstbuffer < srclen)
    dstbuffer = srclen;
  char *dststr = calloc(dstbuffer + 1, sizeof (char));
  char *dptr = dststr;

  if ((ptr = strstr(srcstr, orig)))
    {
      while (ptr < srcend && (ptr = strstr(sptr, orig)))
        {
          int len = ptr - sptr;
          memcpy(dptr, sptr, len);
          sptr += len + origlen;
          dptr += len;
          memcpy(dptr, repl, repllen);
          dptr += repllen;
        }
      memcpy(dptr, sptr, srcend - sptr);
      if (didAlloc)
        free(srcstr);
      return dststr;
    }

  memcpy(dststr, srcstr, srclen);
  if (didAlloc)
    free(srcstr);
  return dststr;
}

AVal
StripParams(AVal *src)
{
  AVal str;
  if (src->av_val)
    {
      str.av_val = calloc(src->av_len + 1, sizeof (char));
      strncpy(str.av_val, src->av_val, src->av_len);
      str.av_len = src->av_len;
      char *start = str.av_val;
      char *end = start + str.av_len;
      char *ptr = start;

      while (ptr < end)
        {
          if (*ptr == '?')
            {
              str.av_len = ptr - start;
              break;
            }
          ptr++;
        }
      memset(start + str.av_len, 0, 1);

      char *dynamic = strstr(start, "[[DYNAMIC]]");
      if (dynamic)
        {
          dynamic -= 1;
          memset(dynamic, 0, 1);
          str.av_len = dynamic - start;
          end = start + str.av_len;
        }

      char *import = strstr(start, "[[IMPORT]]");
      if (import)
        {
          str.av_val = import + 11;
          strcpy(start, "http://");
          str.av_val = strcat(start, str.av_val);
          str.av_len = strlen(str.av_val);
        }
      return str;
    }
  str = *src;
  return str;
}

int
file_exists(const char *fname)
{
  FILE *file;
  if ((file = fopen(fname, "r")))
    {
      fclose(file);
      return TRUE;
    }
  return FALSE;
}

AVal
AVcopy(AVal src)
{
  AVal dst;
  if (src.av_len)
    {
      dst.av_val = malloc(src.av_len + 1);
      memcpy(dst.av_val, src.av_val, src.av_len);
      dst.av_val[src.av_len] = '\0';
      dst.av_len = src.av_len;
    }
  else
    {
      dst.av_val = NULL;
      dst.av_len = 0;
    }
  return dst;
}
