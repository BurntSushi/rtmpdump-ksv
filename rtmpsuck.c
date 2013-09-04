/*  RTMP Proxy Server
 *  Copyright (C) 2009 Andrej Stepanchuk
 *  Copyright (C) 2009 Howard Chu
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

/* This is a Proxy Server that displays the connection parameters from a
 * client and then saves any data streamed to the client.
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

enum
{
  STREAMING_ACCEPTING,
  STREAMING_IN_PROGRESS,
  STREAMING_STOPPING,
  STREAMING_STOPPED
};

typedef struct Flist
{
  struct Flist *f_next;
  FILE *f_file;
  AVal f_path;
} Flist;

typedef struct Plist
{
  struct Plist *p_next;
  RTMPPacket p_pkt;
} Plist;

typedef struct
{
  int socket;
  int state;
  uint32_t stamp;
  RTMP rs;
  RTMP rc;
  Plist *rs_pkt[2];	/* head, tail */
  Plist *rc_pkt[2];	/* head, tail */
  Flist *f_head, *f_tail;
  Flist *f_cur;

} STREAMING_SERVER;

STREAMING_SERVER *rtmpServer = 0;	// server structure pointer

STREAMING_SERVER *startStreaming(const char *address, int port);
void stopStreaming(STREAMING_SERVER * server);

#define STR2AVAL(av,str)	av.av_val = str; av.av_len = strlen(av.av_val)

#ifdef _DEBUG
uint32_t debugTS = 0;

int pnum = 0;

FILE *netstackdump = NULL;
FILE *netstackdump_read = NULL;
#endif

#define BUFFERTIME	(4*60*60*1000)	/* 4 hours */

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
SAVC(play);
SAVC(closeStream);
SAVC(fmsVer);
SAVC(mode);
SAVC(level);
SAVC(code);
SAVC(secureToken);
SAVC(onStatus);
SAVC(close);
SAVC(play2);
static const AVal av_NetStream_Failed = AVC("NetStream.Failed");
static const AVal av_NetStream_Play_Failed = AVC("NetStream.Play.Failed");
static const AVal av_NetStream_Play_StreamNotFound = AVC("NetStream.Play.StreamNotFound");
static const AVal av_NetConnection_Connect_InvalidApp = AVC("NetConnection.Connect.InvalidApp");
static const AVal av_NetStream_Play_Start = AVC("NetStream.Play.Start");
static const AVal av_NetStream_Play_Complete = AVC("NetStream.Play.Complete");
static const AVal av_NetStream_Play_Stop = AVC("NetStream.Play.Stop");
static const AVal av_NetStream_Authenticate_UsherToken = AVC("NetStream.Authenticate.UsherToken");

static const char *cst[] = { "client", "server" };
char *dumpAMF(AMFObject *obj, char *ptr);
char *strreplace(char *srcstr, int srclen, char *orig, char *repl, int didAlloc);
AVal AVcopy(AVal src);
AVal StripParams(AVal *src);

// Returns 0 for OK/Failed/error, 1 for 'Stop or Complete'
int
ServeInvoke(STREAMING_SERVER *server, int which, RTMPPacket *pack, const char *body)
{
  int ret = 0, nRes;
  int nBodySize = pack->m_nBodySize;

  if (body > pack->m_body)
    nBodySize--;

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
  RTMP_Log(RTMP_LOGDEBUG, "%s, %s invoking <%s>", __FUNCTION__, cst[which], method.av_val);

  if (AVMATCH(&method, &av_connect))
    {
      AMFObject cobj;
      AVal pname, pval;
      int i;
      AMFProp_GetObject(AMF_GetProp(&obj, NULL, 2), &cobj);
      RTMP_LogPrintf("Processing connect\n");
      for (i=0; i<cobj.o_num; i++)
        {
          pname = cobj.o_props[i].p_name;
          pval.av_val = NULL;
          pval.av_len = 0;
          if (cobj.o_props[i].p_type == AMF_STRING)
            {
              pval = cobj.o_props[i].p_vu.p_aval;
              RTMP_LogPrintf("%10.*s : %.*s\n", pname.av_len, pname.av_val, pval.av_len, pval.av_val);
            }
          if (AVMATCH(&pname, &av_app))
            {
              server->rc.Link.app = AVcopy(pval);
              pval.av_val = NULL;
            }
          else if (AVMATCH(&pname, &av_flashVer))
            {
              server->rc.Link.flashVer = AVcopy(pval);
              pval.av_val = NULL;
            }
          else if (AVMATCH(&pname, &av_swfUrl))
            {
#ifdef CRYPTO
              if (pval.av_val)
                {
                  AVal swfUrl = StripParams(&pval);
                  RTMP_HashSWF(swfUrl.av_val, &server->rc.Link.SWFSize, (unsigned char *) server->rc.Link.SWFHash, 30);
                }
#endif
              server->rc.Link.swfUrl = AVcopy(pval);
              pval.av_val = NULL;
            }
          else if (AVMATCH(&pname, &av_tcUrl))
            {
              char *r1 = NULL, *r2;
              int len;

              server->rc.Link.tcUrl = AVcopy(pval);
              if ((pval.av_val[0] | 0x40) == 'r' &&
                  (pval.av_val[1] | 0x40) == 't' &&
                  (pval.av_val[2] | 0x40) == 'm' &&
                  (pval.av_val[3] | 0x40) == 'p')
                {
                  if (pval.av_val[4] == ':')
                    {
                      server->rc.Link.protocol = RTMP_PROTOCOL_RTMP;
                      r1 = pval.av_val+7;
                    }
                  else if ((pval.av_val[4] | 0x40) == 'e' && pval.av_val[5] == ':')
                    {
                      server->rc.Link.protocol = RTMP_PROTOCOL_RTMPE;
                      r1 = pval.av_val+8;
                    }
                  r2 = strchr(r1, '/');
		  if (r2)
                    len = r2 - r1;
		  else
		    len = pval.av_len - (r1 - pval.av_val);
                  r2 = malloc(len+1);
                  memcpy(r2, r1, len);
                  r2[len] = '\0';
                  server->rc.Link.hostname.av_val = r2;
                  r1 = strrchr(r2, ':');
                  if (r1)
                    {
		      server->rc.Link.hostname.av_len = r1 - r2;
                      *r1++ = '\0';
                      server->rc.Link.port = atoi(r1);
                    }
                  else
                    {
		      server->rc.Link.hostname.av_len = len;
                      server->rc.Link.port = 1935;
                    }
                }
              pval.av_val = NULL;
            }
          else if (AVMATCH(&pname, &av_pageUrl))
            {
              server->rc.Link.pageUrl = AVcopy(pval);
              pval.av_val = NULL;
            }
          else if (AVMATCH(&pname, &av_audioCodecs))
            {
              server->rc.m_fAudioCodecs = cobj.o_props[i].p_vu.p_number;
            }
          else if (AVMATCH(&pname, &av_videoCodecs))
            {
              server->rc.m_fVideoCodecs = cobj.o_props[i].p_vu.p_number;
            }
          else if (AVMATCH(&pname, &av_objectEncoding))
            {
              server->rc.m_fEncoding = cobj.o_props[i].p_vu.p_number;
              server->rc.m_bSendEncoding = TRUE;
            }
          /* Dup'd a string we didn't recognize? */
          if (pval.av_val)
            free(pval.av_val);
        }

      if (obj.o_num > 3)
        {
          int i = obj.o_num - 3;
          server->rc.Link.extras.o_num = i;
          server->rc.Link.extras.o_props = malloc(i * sizeof (AMFObjectProperty));
          memcpy(server->rc.Link.extras.o_props, obj.o_props + 3, i * sizeof (AMFObjectProperty));
          obj.o_num = 3;
        }

      if (server->rc.Link.extras.o_num)
        {
          server->rc.Link.Extras.av_val = calloc(2048, sizeof (char));
          dumpAMF(&server->rc.Link.extras, server->rc.Link.Extras.av_val);
          server->rc.Link.Extras.av_len = strlen(server->rc.Link.Extras.av_val);
        }

      if (!RTMP_Connect(&server->rc, pack))
        {
          /* failed */
          return 1;
        }
      server->rc.m_bSendCounter = FALSE;

      if (server->rc.Link.extras.o_props)
        {
          AMF_Reset(&server->rc.Link.extras);
        }
    }
  else if (AVMATCH(&method, &av_NetStream_Authenticate_UsherToken))
    {
      AVal usherToken = {0};
      AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &usherToken);
      server->rc.Link.usherToken = AVcopy(usherToken);
      RTMP_LogPrintf("%10s : %.*s\n", "usherToken", server->rc.Link.usherToken.av_len, server->rc.Link.usherToken.av_val);
    }
  else if (AVMATCH(&method, &av_play2))
    {
      RTMP_Log(RTMP_LOGDEBUG, "%s: Detected play2 request\n", __FUNCTION__);
      if (body && nBodySize > 0)
        {
          char* pCmd = (char*) body;
          char* pEnd = pCmd + nBodySize - 4;
          while (pCmd < pEnd)
            {
              if (pCmd[0] == 'p' && pCmd[1] == 'l' && pCmd[2] == 'a' && pCmd[3] == 'y' && pCmd[4] == '2')
                {
                  /* Disable bitrate transition via sending invalid command */
                  pCmd[4] = 'z';
                  break;
                }
              ++pCmd;
            }
        }
    }
  else if (AVMATCH(&method, &av_play))
    {
      Flist *fl;
      AVal av;
      FILE *out;
      char *file, *p, *q;
      char flvHeader[] = { 'F', 'L', 'V', 0x01,
         0x05,                       // video + audio, we finalize later if the value is different
         0x00, 0x00, 0x00, 0x09,
         0x00, 0x00, 0x00, 0x00      // first prevTagSize=0
       };
      int count = 0, flen;

      server->rc.m_stream_id = pack->m_nInfoField2;
      AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &av);
      server->rc.Link.playpath = av;
      if (!av.av_val)
        goto out;

      double StartFlag = 0;
      AMFObjectProperty *Start = AMF_GetProp(&obj, NULL, 4);
      if (!(Start->p_type == AMF_INVALID))
        StartFlag = AMFProp_GetNumber(Start);
      if (StartFlag == -1000 || (server->rc.Link.app.av_val && strstr(server->rc.Link.app.av_val, "live")))
        StartFlag = -1000;
      RTMP_LogPrintf("%10s : %s\n", "live", (StartFlag == -1000) ? "yes" : "no");

      /* check for duplicates */
      for (fl = server->f_head; fl; fl=fl->f_next)
        {
          if (AVMATCH(&av, &fl->f_path))
            count++;
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
      flen = av.av_len;
      /* hope there aren't more than 255 dups */
      if (count)
        flen += 2;
      file = malloc(flen + 5);

      memcpy(file, av.av_val, av.av_len);
      if (count)
        sprintf(file+av.av_len, "%02x", count);
      else
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

      RTMP_LogPrintf("%10s : %.*s\n%10s : %s\n", "Playpath", server->rc.Link.playpath.av_len,
                     server->rc.Link.playpath.av_val, "Saving as", file);

      /* Save command to text file */
      char *cmd = NULL, *ptr = NULL;
      AVal swfUrl, tcUrl;

      cmd = calloc(4096, sizeof (char));
      ptr = cmd;
      tcUrl = StripParams(&server->rc.Link.tcUrl);
      swfUrl = StripParams(&server->rc.Link.swfUrl);
      ptr += sprintf(ptr, "rtmpdump -r \"%.*s\" -a \"%.*s\" -f \"%.*s\" -W \"%.*s\" -p \"%.*s\"",
                     tcUrl.av_len, tcUrl.av_val,
                     server->rc.Link.app.av_len, server->rc.Link.app.av_val,
                     server->rc.Link.flashVer.av_len, server->rc.Link.flashVer.av_val,
                     swfUrl.av_len, swfUrl.av_val,
                     server->rc.Link.pageUrl.av_len, server->rc.Link.pageUrl.av_val);

      if (server->rc.Link.usherToken.av_val)
        {
          char *usherToken = strreplace(server->rc.Link.usherToken.av_val, server->rc.Link.usherToken.av_len, "\"", "\\\"", TRUE);
#ifdef WIN32
          usherToken = strreplace(usherToken, 0, "^", "^^", TRUE);
          usherToken = strreplace(usherToken, 0, "|", "^|", TRUE);
#endif
          ptr += sprintf(ptr, " --jtv \"%s\"", usherToken);
          free(usherToken);
        }

      if (server->rc.Link.Extras.av_len)
        {
          ptr += sprintf(ptr, "%.*s", server->rc.Link.Extras.av_len, server->rc.Link.Extras.av_val);
        }

      if (StartFlag == -1000)
        ptr += sprintf(ptr, "%s", " --live");
      ptr += sprintf(ptr, " -y \"%.*s\"", server->rc.Link.playpath.av_len, server->rc.Link.playpath.av_val);
      ptr += sprintf(ptr, " -o \"%s\"\n", file);

      FILE *cmdfile = fopen("Command.txt", "a");
      fprintf(cmdfile, "%s", cmd);
      fclose(cmdfile);
      free(cmd);

      out = fopen(file, "wb");
      free(file);
      if (!out)
        ret = 1;
      else
        {
          fwrite(flvHeader, 1, sizeof(flvHeader), out);
          av = server->rc.Link.playpath;
          fl = malloc(sizeof(Flist)+av.av_len+1);
          fl->f_file = out;
          fl->f_path.av_len = av.av_len;
          fl->f_path.av_val = (char *)(fl+1);
          memcpy(fl->f_path.av_val, av.av_val, av.av_len);
          fl->f_path.av_val[av.av_len] = '\0';
          fl->f_next = NULL;
          if (server->f_tail)
            server->f_tail->f_next = fl;
          else
            server->f_head = fl;
          server->f_tail = fl;
        }
    }
  else if (AVMATCH(&method, &av_onStatus))
    {
      AMFObject obj2;
      AVal code, level;
      AMFProp_GetObject(AMF_GetProp(&obj, NULL, 3), &obj2);
      AMFProp_GetString(AMF_GetProp(&obj2, &av_code, -1), &code);
      AMFProp_GetString(AMF_GetProp(&obj2, &av_level, -1), &level);

      RTMP_Log(RTMP_LOGDEBUG, "%s, onStatus: %s", __FUNCTION__, code.av_val);
      if (AVMATCH(&code, &av_NetStream_Failed)
	  || AVMATCH(&code, &av_NetStream_Play_Failed)
	  || AVMATCH(&code, &av_NetStream_Play_StreamNotFound)
	  || AVMATCH(&code, &av_NetConnection_Connect_InvalidApp))
	{
	  ret = 1;
	}

      if (AVMATCH(&code, &av_NetStream_Play_Start))
	{
          /* set up the next stream */
          if (server->f_cur)
		    {
		      if (server->f_cur->f_next)
                server->f_cur = server->f_cur->f_next;
			}
          else
            {
              for (server->f_cur = server->f_head; server->f_cur &&
                    !server->f_cur->f_file; server->f_cur = server->f_cur->f_next) ;
            }
	  server->rc.m_bPlaying = TRUE;
	}

      // Return 1 if this is a Play.Complete or Play.Stop
      if (AVMATCH(&code, &av_NetStream_Play_Complete)
	  || AVMATCH(&code, &av_NetStream_Play_Stop))
	{
	  ret = 1;
	}
    }
  else if (AVMATCH(&method, &av_closeStream))
    {
      ret = 1;
    }
  else if (AVMATCH(&method, &av_close))
    {
      RTMP_Close(&server->rc);
      ret = 1;
    }
out:
  AMF_Reset(&obj);
  return ret;
}

int
ServePacket(STREAMING_SERVER *server, int which, RTMPPacket *packet)
{
  int ret = 0;

  RTMP_Log(RTMP_LOGDEBUG, "%s, %s sent packet type %02X, size %u bytes", __FUNCTION__,
    cst[which], packet->m_packetType, packet->m_nBodySize);

  switch (packet->m_packetType)
    {
    case RTMP_PACKET_TYPE_CHUNK_SIZE:
      // chunk size
//      HandleChangeChunkSize(r, packet);
      break;

    case RTMP_PACKET_TYPE_BYTES_READ_REPORT:
      // bytes read report
      break;

    case RTMP_PACKET_TYPE_CONTROL:
      // ctrl
//      HandleCtrl(r, packet);
      break;

    case RTMP_PACKET_TYPE_SERVER_BW:
      // server bw
//      HandleServerBW(r, packet);
      break;

    case RTMP_PACKET_TYPE_CLIENT_BW:
      // client bw
 //     HandleClientBW(r, packet);
      break;

    case RTMP_PACKET_TYPE_AUDIO:
      // audio data
      //RTMP_Log(RTMP_LOGDEBUG, "%s, received: audio %lu bytes", __FUNCTION__, packet.m_nBodySize);
      break;

    case RTMP_PACKET_TYPE_VIDEO:
      // video data
      //RTMP_Log(RTMP_LOGDEBUG, "%s, received: video %lu bytes", __FUNCTION__, packet.m_nBodySize);
      break;

    case RTMP_PACKET_TYPE_FLEX_STREAM_SEND:
      // flex stream send
      break;

    case RTMP_PACKET_TYPE_FLEX_SHARED_OBJECT:
      // flex shared object
      break;

    case RTMP_PACKET_TYPE_FLEX_MESSAGE:
      // flex message
      {
	ret = ServeInvoke(server, which, packet, packet->m_body + 1);
	break;
      }
    case RTMP_PACKET_TYPE_INFO:
      // metadata (notify)
      break;

    case RTMP_PACKET_TYPE_SHARED_OBJECT:
      /* shared object */
      break;

    case RTMP_PACKET_TYPE_INVOKE:
      // invoke
      ret = ServeInvoke(server, which, packet, packet->m_body);
      break;

    case RTMP_PACKET_TYPE_FLASH_VIDEO:
      /* flv */
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

int
WriteStream(char **buf,	// target pointer, maybe preallocated
	    unsigned int *plen,	// length of buffer if preallocated
            uint32_t *nTimeStamp,
            RTMPPacket *packet)
{
  uint32_t prevTagSize = 0;
  int ret = -1, len = *plen;

  while (1)
    {
      char *packetBody = packet->m_body;
      unsigned int nPacketLen = packet->m_nBodySize;

      // skip video info/command packets
      if (packet->m_packetType == RTMP_PACKET_TYPE_VIDEO &&
	  nPacketLen == 2 && ((*packetBody & 0xf0) == 0x50))
	{
	  ret = 0;
	  break;
	}

      if (packet->m_packetType == RTMP_PACKET_TYPE_VIDEO && nPacketLen <= 5)
	{
	  RTMP_Log(RTMP_LOGWARNING, "ignoring too small video packet: size: %d",
	      nPacketLen);
	  ret = 0;
	  break;
	}
      if (packet->m_packetType == RTMP_PACKET_TYPE_AUDIO && nPacketLen <= 1)
	{
	  RTMP_Log(RTMP_LOGWARNING, "ignoring too small audio packet: size: %d",
	      nPacketLen);
	  ret = 0;
	  break;
	}
#ifdef _DEBUG
      RTMP_Log(RTMP_LOGDEBUG, "type: %02X, size: %d, TS: %d ms", packet->m_packetType,
	  nPacketLen, packet->m_nTimeStamp);
      if (packet->m_packetType == RTMP_PACKET_TYPE_VIDEO)
	RTMP_Log(RTMP_LOGDEBUG, "frametype: %02X", (*packetBody & 0xf0));
#endif

      // calculate packet size and reallocate buffer if necessary
      unsigned int size = nPacketLen
	+
	((packet->m_packetType == RTMP_PACKET_TYPE_AUDIO
          || packet->m_packetType == RTMP_PACKET_TYPE_VIDEO
	  || packet->m_packetType == RTMP_PACKET_TYPE_INFO) ? 11 : 0)
        + (packet->m_packetType != 0x16 ? 4 : 0);

      if (size + 4 > len)
	{
          /* The extra 4 is for the case of an FLV stream without a last
           * prevTagSize (we need extra 4 bytes to append it).  */
	  *buf = (char *) realloc(*buf, size + 4);
	  if (*buf == 0)
	    {
	      RTMP_Log(RTMP_LOGERROR, "Couldn't reallocate memory!");
	      ret = -1;		// fatal error
	      break;
	    }
	}
      char *ptr = *buf, *pend = ptr + size+4;

      /* audio (RTMP_PACKET_TYPE_AUDIO), video (RTMP_PACKET_TYPE_VIDEO)
       * or metadata (RTMP_PACKET_TYPE_INFO) packets: construct 11 byte
       * header then add rtmp packet's data.  */
      if (packet->m_packetType == RTMP_PACKET_TYPE_AUDIO
          || packet->m_packetType == RTMP_PACKET_TYPE_VIDEO
	  || packet->m_packetType == RTMP_PACKET_TYPE_INFO)
	{
	  // set data type
	  //*dataType |= (((packet->m_packetType == RTMP_PACKET_TYPE_AUDIO)<<2)|(packet->m_packetType == RTMP_PACKET_TYPE_VIDEO));

	  (*nTimeStamp) = packet->m_nTimeStamp;
	  prevTagSize = 11 + nPacketLen;

	  *ptr++ = packet->m_packetType;
	  ptr = AMF_EncodeInt24(ptr, pend, nPacketLen);
	  ptr = AMF_EncodeInt24(ptr, pend, *nTimeStamp);
	  *ptr = (char) (((*nTimeStamp) & 0xFF000000) >> 24);
	  ptr++;

	  // stream id
	  ptr = AMF_EncodeInt24(ptr, pend, 0);
	}

      memcpy(ptr, packetBody, nPacketLen);
      unsigned int len = nPacketLen;

      // correct tagSize and obtain timestamp if we have an FLV stream
      if (packet->m_packetType == RTMP_PACKET_TYPE_FLASH_VIDEO)
	{
	  unsigned int pos = 0;

	  while (pos + 11 < nPacketLen)
	    {
	      uint32_t dataSize = AMF_DecodeInt24(packetBody + pos + 1);	// size without header (11) and without prevTagSize (4)
	      *nTimeStamp = AMF_DecodeInt24(packetBody + pos + 4);
	      *nTimeStamp |= (packetBody[pos + 7] << 24);

#if 0
	      /* set data type */
	      *dataType |= (((*(packetBody+pos) == RTMP_PACKET_TYPE_AUDIO) << 2)
                            | (*(packetBody+pos) == RTMP_PACKET_TYPE_VIDEO));
#endif

	      if (pos + 11 + dataSize + 4 > nPacketLen)
		{
		  if (pos + 11 + dataSize > nPacketLen)
		    {
		      RTMP_Log(RTMP_LOGERROR,
			  "Wrong data size (%u), stream corrupted, aborting!",
			  dataSize);
		      ret = -2;
		      break;
		    }
		  RTMP_Log(RTMP_LOGWARNING, "No tagSize found, appending!");

		  // we have to append a last tagSize!
		  prevTagSize = dataSize + 11;
		  AMF_EncodeInt32(ptr + pos + 11 + dataSize, pend, prevTagSize);
		  size += 4;
		  len += 4;
		}
	      else
		{
		  prevTagSize =
		    AMF_DecodeInt32(packetBody + pos + 11 + dataSize);

#ifdef _DEBUG
		  RTMP_Log(RTMP_LOGDEBUG,
		      "FLV Packet: type %02X, dataSize: %lu, tagSize: %lu, timeStamp: %lu ms",
		      (unsigned char) packetBody[pos], dataSize, prevTagSize,
		      *nTimeStamp);
#endif

		  if (prevTagSize != (dataSize + 11))
		    {
#ifdef _DEBUG
		      RTMP_Log(RTMP_LOGWARNING,
			  "Tag and data size are not consitent, writing tag size according to dataSize+11: %d",
			  dataSize + 11);
#endif

		      prevTagSize = dataSize + 11;
		      AMF_EncodeInt32(ptr + pos + 11 + dataSize, pend, prevTagSize);
		    }
		}

	      pos += prevTagSize + 4;	//(11+dataSize+4);
	    }
	}
      ptr += len;

      if (packet->m_packetType != RTMP_PACKET_TYPE_FLASH_VIDEO)
	{			// FLV tag packets contain their own prevTagSize
	  AMF_EncodeInt32(ptr, pend, prevTagSize);
	  //ptr += 4;
	}

      ret = size;
      break;
    }

  if (len > *plen)
    *plen = len;

  return ret;			// no more media packets
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

TFTYPE doServe(void *arg)	// server socket and state (our listening socket)
{
  STREAMING_SERVER *server = arg;
  RTMPPacket pc = { 0 }, ps = { 0 };
  RTMPChunk rk = { 0 };
  char *buf = NULL;
  unsigned int buflen = 131072;
  int paused = FALSE;
  int sockfd = server->socket;

  // timeout for http requests
  fd_set rfds;
  struct timeval tv;

  server->state = STREAMING_IN_PROGRESS;

  memset(&tv, 0, sizeof(struct timeval));
  tv.tv_sec = 5;

  FD_ZERO(&rfds);
  FD_SET(sockfd, &rfds);

  if (select(sockfd + 1, &rfds, NULL, NULL, &tv) <= 0)
    {
      RTMP_Log(RTMP_LOGERROR, "Request timeout/select failed, ignoring request");
      goto quit;
    }
  else
    {
      RTMP_Init(&server->rs);
      RTMP_Init(&server->rc);
      server->rs.m_sb.sb_socket = sockfd;
      if (!RTMP_Serve(&server->rs))
        {
          RTMP_Log(RTMP_LOGERROR, "Handshake failed");
          goto cleanup;
        }
    }

  buf = malloc(buflen);

  /* Just process the Connect request */
  while (RTMP_IsConnected(&server->rs) && RTMP_ReadPacket(&server->rs, &ps))
    {
      if (!RTMPPacket_IsReady(&ps))
        continue;
      ServePacket(server, 0, &ps);
      RTMPPacket_Free(&ps);
      if (RTMP_IsConnected(&server->rc))
        break;
    }

  pc.m_chunk = &rk;

  /* We have our own timeout in select() */
  server->rc.Link.timeout = 10;
  server->rs.Link.timeout = 10;
  while (RTMP_IsConnected(&server->rs) || RTMP_IsConnected(&server->rc))
    {
      int n;
      int sr, cr;

      cr = server->rc.m_sb.sb_size;
      sr = server->rs.m_sb.sb_size;

      if (cr || sr)
        {
        }
      else
        {
          n = server->rs.m_sb.sb_socket;
	  if (server->rc.m_sb.sb_socket > n)
	    n = server->rc.m_sb.sb_socket;
	  FD_ZERO(&rfds);
	  if (RTMP_IsConnected(&server->rs))
	    FD_SET(sockfd, &rfds);
	  if (RTMP_IsConnected(&server->rc))
	    FD_SET(server->rc.m_sb.sb_socket, &rfds);

          /* give more time to start up if we're not playing yet */
	  tv.tv_sec = server->f_cur ? 30 : 60;
	  tv.tv_usec = 0;

	  if (select(n + 1, &rfds, NULL, NULL, &tv) <= 0)
	    {
              if (server->f_cur && server->rc.m_mediaChannel && !paused)
                {
                  server->rc.m_pauseStamp = server->rc.m_channelTimestamp[server->rc.m_mediaChannel];
                  if (RTMP_ToggleStream(&server->rc))
                    {
                      paused = TRUE;
                      continue;
                    }
                }
	      RTMP_Log(RTMP_LOGERROR, "Request timeout/select failed, ignoring request");
	      goto cleanup;
	    }
          if (server->rs.m_sb.sb_socket > 0 &&
	    FD_ISSET(server->rs.m_sb.sb_socket, &rfds))
            sr = 1;
          if (server->rc.m_sb.sb_socket > 0 &&
	    FD_ISSET(server->rc.m_sb.sb_socket, &rfds))
            cr = 1;
        }
      if (sr)
        {
          while (RTMP_ReadPacket(&server->rs, &ps))
            if (RTMPPacket_IsReady(&ps))
              {
                /* change chunk size */
                if (ps.m_packetType == RTMP_PACKET_TYPE_CHUNK_SIZE)
                  {
                    if (ps.m_nBodySize >= 4)
                      {
                        server->rs.m_inChunkSize = AMF_DecodeInt32(ps.m_body);
                        RTMP_Log(RTMP_LOGDEBUG, "%s, client: chunk size change to %d", __FUNCTION__,
                            server->rs.m_inChunkSize);
                        server->rc.m_outChunkSize = server->rs.m_inChunkSize;
                      }
                  }
                /* bytes received */
                else if (ps.m_packetType == RTMP_PACKET_TYPE_BYTES_READ_REPORT)
                  {
                    if (ps.m_nBodySize >= 4)
                      {
                        int count = AMF_DecodeInt32(ps.m_body);
                        RTMP_Log(RTMP_LOGDEBUG, "%s, client: bytes received = %d", __FUNCTION__,
                            count);
                      }
                  }
                /* ctrl */
                else if (ps.m_packetType == RTMP_PACKET_TYPE_CONTROL)
                  {
                    short nType = AMF_DecodeInt16(ps.m_body);
                    /* UpdateBufferMS */
                    if (nType == 0x03)
                      {
                        char *ptr = ps.m_body+2;
                        int id;
                        int len;
                        id = AMF_DecodeInt32(ptr);
                        /* Assume the interesting media is on a non-zero stream */
                        if (id)
                          {
                            len = AMF_DecodeInt32(ptr+4);
#if 1
                            /* request a big buffer */
                            if (len < BUFFERTIME)
                              {
                                AMF_EncodeInt32(ptr+4, ptr+8, BUFFERTIME);
                              }
#endif
                            RTMP_Log(RTMP_LOGDEBUG, "%s, client: BufferTime change in stream %d to %d", __FUNCTION__,
                                id, len);
                          }
                      }
                  }
                else if (ps.m_packetType == RTMP_PACKET_TYPE_FLEX_MESSAGE
                         || ps.m_packetType == RTMP_PACKET_TYPE_INVOKE)
                  {
                    if (ServePacket(server, 0, &ps) && server->f_cur)
                      {
                        fclose(server->f_cur->f_file);
                        server->f_cur->f_file = NULL;
                        server->f_cur = NULL;
                      }
                  }
                RTMP_SendPacket(&server->rc, &ps, FALSE);
                RTMPPacket_Free(&ps);
                break;
              }
        }
      if (cr)
        {
          while (RTMP_ReadPacket(&server->rc, &pc))
            {
              int sendit = 1;
              if (RTMPPacket_IsReady(&pc))
                {
                  if (paused)
                    {
                      if (pc.m_nTimeStamp <= server->rc.m_mediaStamp)
                        continue;
                      paused = 0;
                      server->rc.m_pausing = 0;
                    }
                  /* change chunk size */
                  if (pc.m_packetType == RTMP_PACKET_TYPE_CHUNK_SIZE)
                    {
                      if (pc.m_nBodySize >= 4)
                        {
                          server->rc.m_inChunkSize = AMF_DecodeInt32(pc.m_body);
                          RTMP_Log(RTMP_LOGDEBUG, "%s, server: chunk size change to %d", __FUNCTION__,
                              server->rc.m_inChunkSize);
                          server->rs.m_outChunkSize = server->rc.m_inChunkSize;
                        }
                    }
                  else if (pc.m_packetType == RTMP_PACKET_TYPE_CONTROL)
                    {
                      short nType = AMF_DecodeInt16(pc.m_body);
                      /* SWFverification */
                      if (nType == 0x1a)
#ifdef CRYPTO
                        if (server->rc.Link.SWFSize)
                        {
                          RTMP_SendCtrl(&server->rc, 0x1b, 0, 0);
                          sendit = 0;
                        }
#else
                        /* The session will certainly fail right after this */
                        RTMP_Log(RTMP_LOGERROR, "%s, server requested SWF verification, need CRYPTO support! ", __FUNCTION__);
#endif
                    }
                  else if (server->f_cur && (
                       pc.m_packetType == RTMP_PACKET_TYPE_AUDIO ||
                       pc.m_packetType == RTMP_PACKET_TYPE_VIDEO ||
                       pc.m_packetType == RTMP_PACKET_TYPE_INFO ||
                       pc.m_packetType == RTMP_PACKET_TYPE_FLASH_VIDEO) &&
                       RTMP_ClientPacket(&server->rc, &pc))
                    {
                      int len = WriteStream(&buf, &buflen, &server->stamp, &pc);
                      if (len > 0 && fwrite(buf, 1, len, server->f_cur->f_file) != len)
                        goto cleanup;
                    }
                  else if (pc.m_packetType == RTMP_PACKET_TYPE_FLEX_MESSAGE ||
                           pc.m_packetType == RTMP_PACKET_TYPE_INVOKE)
                    {
                      if (ServePacket(server, 1, &pc) && server->f_cur)
                        {
                          fclose(server->f_cur->f_file);
                          server->f_cur->f_file = NULL;
                          server->f_cur = NULL;
                        }
                    }
                }
              if (sendit && RTMP_IsConnected(&server->rs))
                RTMP_SendChunk(&server->rs, &rk);
              if (RTMPPacket_IsReady(&pc))
                  RTMPPacket_Free(&pc);
              break;
            }
        }
      if (!RTMP_IsConnected(&server->rs) && RTMP_IsConnected(&server->rc)
        && !server->f_cur)
        RTMP_Close(&server->rc);
    }

cleanup:
  RTMP_LogPrintf("Closing connection... ");
  RTMP_Close(&server->rs);
  RTMP_Close(&server->rc);
  while (server->f_head)
    {
      Flist *fl = server->f_head;
      server->f_head = fl->f_next;
      if (fl->f_file)
        fclose(fl->f_file);
      free(fl);
    }
  server->f_tail = NULL;
  server->f_cur = NULL;
  free(buf);
  /* Should probably be done by RTMP_Close() ... */
  server->rc.Link.hostname.av_val = NULL;
  server->rc.Link.tcUrl.av_val = NULL;
  server->rc.Link.swfUrl.av_val = NULL;
  server->rc.Link.pageUrl.av_val = NULL;
  server->rc.Link.app.av_val = NULL;
  server->rc.Link.auth.av_val = NULL;
  server->rc.Link.flashVer.av_val = NULL;
  RTMP_LogPrintf("done!\n\n");

quit:
  if (server->state == STREAMING_IN_PROGRESS)
    server->state = STREAMING_ACCEPTING;

  TFRET();
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
      STREAMING_SERVER *srv2 = malloc(sizeof(STREAMING_SERVER));
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
	  *srv2 = *server;
	  srv2->socket = sockfd;
	  /* Create a new thread and transfer the control to that */
	  ThreadCreate(doServe, srv2);
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
      int fd = server->socket;
      server->socket = 0;
      if (server->state == STREAMING_IN_PROGRESS)
	{
	  server->state = STREAMING_STOPPING;

	  // wait for streaming threads to exit
	  while (server->state != STREAMING_STOPPED)
	    msleep(1);
	}

      if (fd && closesocket(fd))
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

  // rtmp streaming server
  char DEFAULT_RTMP_STREAMING_DEVICE[] = "0.0.0.0";	// 0.0.0.0 is any device

  char *rtmpStreamingDevice = DEFAULT_RTMP_STREAMING_DEVICE;	// streaming device, default 0.0.0.0
  int nRtmpStreamingPort = 1935;	// port

  RTMP_LogPrintf("RTMP Proxy Server %s\n", RTMPDUMP_VERSION);
  RTMP_LogPrintf("(c) 2010 Andrej Stepanchuk, Howard Chu; license: GPL\n\n");

  RTMP_debuglevel = RTMP_LOGINFO;

  if (argc > 1 && !strcmp(argv[1], "-z"))
    RTMP_debuglevel = RTMP_LOGALL;

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

  free(rtmpServer);

  CleanupSockets();

#ifdef _DEBUG
  if (netstackdump != 0)
    fclose(netstackdump);
  if (netstackdump_read != 0)
    fclose(netstackdump_read);
#endif
  return nStatus;
}

char *
dumpAMF(AMFObject *obj, char *ptr)
{
  int i;
  const char opt[] = "NBSO Z";

  for (i = 0; i < obj->o_num; i++)
    {
      AMFObjectProperty *p = &obj->o_props[i];
      if ((p->p_type == AMF_ECMA_ARRAY) || (p->p_type == AMF_STRICT_ARRAY))
        p->p_type = AMF_OBJECT;
      if (p->p_type > 5)
        continue;
      ptr += sprintf(ptr, " -C ");
      if (p->p_name.av_val)
        *ptr++ = 'N';
      *ptr++ = opt[p->p_type];
      *ptr++ = ':';
      if (p->p_name.av_val)
        ptr += sprintf(ptr, "%.*s:", p->p_name.av_len, p->p_name.av_val);
      switch (p->p_type)
        {
        case AMF_BOOLEAN:
          *ptr++ = p->p_vu.p_number != 0 ? '1' : '0';
          break;
        case AMF_STRING:
          memcpy(ptr, p->p_vu.p_aval.av_val, p->p_vu.p_aval.av_len);
          ptr += p->p_vu.p_aval.av_len;
          break;
        case AMF_NUMBER:
          ptr += sprintf(ptr, "%f", p->p_vu.p_number);
          break;
        case AMF_OBJECT:
          *ptr++ = '1';
          ptr = dumpAMF(&p->p_vu.p_object, ptr);
          ptr += sprintf(ptr, " -C O:0");
          break;
        case AMF_NULL:
        default:
          break;
        }
    }
  return ptr;
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
