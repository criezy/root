//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClient                                                            //
//                                                                      //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// A UNIX reference client for xrootd.                                  //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//         $Id$

#include "XrdClient/XrdClient.hh"
#include "XrdClient/XrdClientDebug.hh"
#include "XrdClient/XrdClientUrlSet.hh"
#include "XrdClient/XrdClientConn.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "XrdClient/XrdClientConnMgr.hh"
#include "XrdClient/XrdClientSid.hh"
#include "XrdClient/XrdClientMStream.hh"
#include "XrdClient/XrdClientReadV.hh"
#include "XrdOuc/XrdOucCRC.hh"

#include <stdio.h>
#ifndef WIN32
#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>


XrdSysSemWait     XrdClient::fConcOpenSem(DFLT_MAXCONCURRENTOPENS);

//_____________________________________________________________________________
// Calls the Open func in order to parallelize the Open requests
//
void *FileOpenerThread(void *arg, XrdClientThread *thr) {
   // Function executed in the garbage collector thread
   XrdClient *thisObj = (XrdClient *)arg;

   thr->SetCancelDeferred();
   thr->SetCancelOn();

   thisObj->TryOpen(thisObj->fOpenPars.mode, thisObj->fOpenPars.options, false);

   return 0;
}


//_____________________________________________________________________________
XrdClient::XrdClient(const char *url) {
    fReadAheadLast = 0;
    fOpenerTh = 0;
    fOpenProgCnd = new XrdSysCondVar(0);
    fReadWaitData = new XrdSysCondVar(0);

    memset(&fStatInfo, 0, sizeof(fStatInfo));
    memset(&fOpenPars, 0, sizeof(fOpenPars));

    // Pick-up the latest setting of the debug level
    DebugSetLevel(EnvGetLong(NAME_DEBUG));

    if (!ConnectionManager)
	Info(XrdClientDebug::kUSERDEBUG,
	     "Create",
	     "(C) 2004-2010 by the Xrootd group. XrdClient $Revision: 1.142 $ - Xrootd version: " << XrdVSTRING);

#ifndef WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    fInitialUrl = url;

    fConnModule = new XrdClientConn();


    if (!fConnModule) {
	Error("Create","Object creation failed.");
	abort();
    }

    fConnModule->SetRedirHandler(this);

    int CacheSize = EnvGetLong(NAME_READCACHESIZE);
    int RaSize = EnvGetLong(NAME_READAHEADSIZE);
    int RmPolicy = EnvGetLong(NAME_READCACHEBLKREMPOLICY);
    fUseCache = (CacheSize > 0);
    SetCacheParameters(CacheSize, RaSize, RmPolicy);
}

//_____________________________________________________________________________
XrdClient::~XrdClient()
{

   if (IsOpen_wait()) Close();

   // Terminate the opener thread
   fOpenProgCnd->Lock();

   if (fOpenerTh) {
      fOpenerTh->Cancel();
      fOpenerTh->Join();
      delete fOpenerTh;
      fOpenerTh = 0;
   }

   fOpenProgCnd->UnLock(); 

   if (fConnModule)
      delete fConnModule;

   delete fReadWaitData;
   delete fOpenProgCnd;
}

//_____________________________________________________________________________
bool XrdClient::IsOpen_inprogress()
{
   // Non blocking access to the 'inprogress' flag
   bool res;

   if (!fOpenProgCnd) return false;

   fOpenProgCnd->Lock();
   res = fOpenPars.inprogress;
   fOpenProgCnd->UnLock();

   return res;
};

//_____________________________________________________________________________
bool XrdClient::IsOpen_wait() {
    bool res;

    if (!fOpenProgCnd) return false;

    fOpenProgCnd->Lock();

    if (fOpenPars.inprogress) {
	fOpenProgCnd->Wait();
	if (fOpenerTh) {
            fOpenerTh->Join();
	    delete fOpenerTh;
	    fOpenerTh = 0;
	}
    }
    res = fOpenPars.opened;
    fOpenProgCnd->UnLock();

    return res;
};

//_____________________________________________________________________________
void XrdClient::TerminateOpenAttempt() {
    fOpenProgCnd->Lock();

    fOpenPars.inprogress = false;
    fOpenProgCnd->Broadcast();
    fOpenProgCnd->UnLock();

    fConcOpenSem.Post();

    //cout << "Mytest " << time(0) << " File: " << fUrl.File << " - Open finished." << endl;
}

//_____________________________________________________________________________
bool XrdClient::Open(kXR_unt16 mode, kXR_unt16 options, bool doitparallel) {
    short locallogid;
  
    // But we initialize the internal params...
    fOpenPars.opened = FALSE;  
    fOpenPars.options = options;
    fOpenPars.mode = mode;  

    // Now we try to set up the first connection
    // We cycle through the list of urls given in fInitialUrl
  

    // Max number of tries
    int connectMaxTry = EnvGetLong(NAME_FIRSTCONNECTMAXCNT);
  
    // Construction of the url set coming from the resolution of the hosts given
    XrdClientUrlSet urlArray(fInitialUrl);
    if (!urlArray.IsValid()) {
	Error("Open", "The URL provided is incorrect.");
	return FALSE;
    }

    XrdClientUrlInfo unfo(fInitialUrl);
    if (unfo.File == "") {
      Error("Open", "The URL provided is incorrect.");
      return FALSE;
    }
    

    //
    // Now start the connection phase, picking randomly from UrlArray
    //
    urlArray.Rewind();
    locallogid = -1;
    int urlstried = 0;
    for (int connectTry = 0;
	 (connectTry < connectMaxTry) && (!fConnModule->IsConnected()); 
	 connectTry++) {

	XrdClientUrlInfo *thisUrl = 0;
	urlstried = (urlstried == urlArray.Size()) ? 0 : urlstried;

	bool nogoodurl = TRUE;
	while (urlArray.Size() > 0) {

	  unsigned int seed = XrdOucCRC::CRC32((const unsigned char*)unfo.File.c_str(), unfo.File.length());

	    // Get an url from the available set
	    if ((thisUrl = urlArray.GetARandomUrl(seed))) {

		if (fConnModule->CheckHostDomain(thisUrl->Host)) {
		    nogoodurl = FALSE;

		    Info(XrdClientDebug::kHIDEBUG, "Open", "Trying to connect to " <<
			 thisUrl->Host << ":" << thisUrl->Port << ". Connect try " <<
			 connectTry+1);
		    locallogid = fConnModule->Connect(*thisUrl, this);
		    // To find out if we have tried the whole URLs set
		    urlstried++;
		    break;
		} else {
		    // Invalid domain: drop the url and move to next, if any
		    urlArray.EraseUrl(thisUrl);
		    continue;
		}
	    }
	}
	if (nogoodurl) {
	    Error("Open", "Access denied to all URL domains requested");
	    break;
	}

	// We are connected to a host. Let's handshake with it.
	if (fConnModule->IsConnected()) {

	    // Now the have the logical Connection ID, that we can use as streamid for 
	    // communications with the server

	    Info(XrdClientDebug::kHIDEBUG, "Open",
		 "The logical connection id is " << fConnModule->GetLogConnID() <<
		 ".");

	    fConnModule->SetUrl(*thisUrl);
	    fUrl = *thisUrl;
        
	    Info(XrdClientDebug::kHIDEBUG, "Open", "Working url is " << thisUrl->GetUrl());
        
	    // after connection deal with server
	    if (!fConnModule->GetAccessToSrv()) {

               if (fConnModule->GetRedirCnt() >= fConnModule->GetMaxRedirCnt()) {
                  // We have been so unlucky.
                  // The max number of redirections was exceeded while logging in
                  fConnModule->Disconnect(TRUE);
                  Error("Open", "Access to server failed: Max redirections exceeded. This means typically 'too many errors'.");
                  break;
               }

		if (fConnModule->LastServerError.errnum == kXR_NotAuthorized) {
		    if (urlstried == urlArray.Size()) {
			// Authentication error: we tried all the indicated URLs:
			// does not make much sense to retry
			fConnModule->Disconnect(TRUE);
			XrdOucString msg(fConnModule->LastServerError.errmsg);
			msg.erasefromend(1);
			Error("Open", "Authentication failure: " << msg);
                        connectTry = connectMaxTry;
		    } else {
			XrdOucString msg(fConnModule->LastServerError.errmsg);
			msg.erasefromend(1);
			Info(XrdClientDebug::kHIDEBUG, "Open",
			     "Authentication failure: " << msg);
		    }
		} else {
                   fConnModule->Disconnect(TRUE);
		    Error("Open", "Access to server failed: error: " <<
			  fConnModule->LastServerError.errnum << " (" << 
			  fConnModule->LastServerError.errmsg << ") - retrying.");
		}
            }
	    else {
		Info(XrdClientDebug::kUSERDEBUG, "Open", "Access to server granted.");
		break;
	    }
	}
     
	// The server denied access. We have to disconnect.
	Info(XrdClientDebug::kHIDEBUG, "Open", "Disconnecting.");
     
	fConnModule->Disconnect(FALSE);
     
	if (connectTry < connectMaxTry-1) {

	    if (DebugLevel() >= XrdClientDebug::kUSERDEBUG)
		Info(XrdClientDebug::kUSERDEBUG, "Open",
		     "Connection attempt failed. Sleeping " <<
		     EnvGetLong(NAME_RECONNECTTIMEOUT) << " seconds.");
     
	    sleep(EnvGetLong(NAME_RECONNECTTIMEOUT));

	}

    } //for connect try

    if (!fConnModule->IsConnected()) {
	return FALSE;
    }

  
    //
    // Variable initialization
    // If the server is a new xrootd ( load balancer or data server)
    //
    if ((fConnModule->GetServerType() != kSTRootd) && 
	(fConnModule->GetServerType() != kSTNone)) {
	// Now we are connected to a server that didn't redirect us after the 
	// login/auth phase
	// let's continue with the openfile sequence

	Info(XrdClientDebug::kUSERDEBUG,
	     "Open", "Opening the remote file " << fUrl.File); 

	if (!TryOpen(mode, options, doitparallel)) {
	    Error("Open", "Error opening the file " <<
		  fUrl.File << " on host " << fUrl.Host << ":" <<
		  fUrl.Port);

	    return FALSE;

	} else {

	    if (doitparallel) {
		Info(XrdClientDebug::kUSERDEBUG, "Open", "File open in progress.");
	    }
	    else
		Info(XrdClientDebug::kUSERDEBUG, "Open", "File opened succesfully.");

	}

    } else {
	// the server is an old rootd
	if (fConnModule->GetServerType() == kSTRootd) {
	    return FALSE;
	}
	if (fConnModule->GetServerType() == kSTNone) {
	    return FALSE;
	}
    }


    return TRUE;

}

//_____________________________________________________________________________
int XrdClient::Read(void *buf, long long offset, int len) {
    XrdClientIntvList cacheholes;
    long blkstowait;
    char *tmpbuf = (char *)buf;

    Info( XrdClientDebug::kHIDEBUG, "Read",
	  "Read(offs=" << offset <<
	  ", len=" << len << ")" );

    if (!IsOpen_wait()) {
	Error("Read", "File not opened.");
	return 0;
    }


    int cachesize = 0;
    long long cachebytessubmitted = 0;
    long long cachebyteshit = 0;
    long long cachemisscount = 0;
    float cachemissrate = 0.0;
    long long cachereadreqcnt = 0;
    float cachebytesusefulness = 0.0;
    bool cachegood = fConnModule->GetCacheInfo(cachesize, cachebytessubmitted,
					       cachebyteshit, cachemisscount,
					       cachemissrate, cachereadreqcnt,
					       cachebytesusefulness);


    // Note: old servers do not support unsolicited responses for reads
    // We also use the plain sync reading if the size of the block is excessive
    // or no cache at all is used
    if (!fUseCache || !cachegood ||
	(cachesize < len) ||
	(fConnModule->GetServerProtocol() < 0x00000270) ) {
	// Without caching

	// Prepare a request header 
	ClientRequest readFileRequest;
	memset( &readFileRequest, 0, sizeof(readFileRequest) );
	fConnModule->SetSID(readFileRequest.header.streamid);
	readFileRequest.read.requestid = kXR_read;
	memcpy( readFileRequest.read.fhandle, fHandle, sizeof(fHandle) );
	readFileRequest.read.offset = offset;
	readFileRequest.read.rlen = len;
	readFileRequest.read.dlen = 0;

	fConnModule->SendGenCommand(&readFileRequest, 0, 0, (void *)buf,
				    FALSE, (char *)"ReadBuffer");

	return fConnModule->LastServerResp.dlen;
    }


    // Ok, from now on we are sure that we have to deal with the cache

    struct XrdClientStatInfo stinfo;
    Stat(&stinfo);
    len = xrdmax(0, xrdmin(len, stinfo.size - offset));

    bool retrysync = false;
    long totbytes = 0;

    
	

	// we cycle until we get all the needed data
	do {
 	    fReadWaitData->Lock();

	    cacheholes.Clear();
	    blkstowait = 0;
	    long bytesgot = 0;


	    


	    if (!retrysync) {



	      

		bytesgot = fConnModule->GetDataFromCache(tmpbuf+totbytes, offset + totbytes,
							 len + offset - 1,
							 true,
							 cacheholes, blkstowait);

                totbytes += bytesgot;

		Info(XrdClientDebug::kHIDEBUG, "Read",
		     "Cache response: got " << bytesgot << "@" << offset + totbytes << " bytes. Holes= " <<
		     cacheholes.GetSize() << " Outstanding= " << blkstowait);

		// If the cache gives the data to us
		//  we don't need to ask the server for them... in principle!
		if( bytesgot >= len ) {

		    // The cache gave us all the requested data

		    Info(XrdClientDebug::kHIDEBUG, "Read",
			 "Found data in cache. len=" << len <<
			 " offset=" << offset);

		    // Are we using read ahead?
		    // We read ahead only if (offs+len) lies in an interval of fReadAheadLast not bigger than the readahead size
		    if ( (fReadAheadLast - (offset+len) < fReadAheadSize) &&
			 (fReadAheadLast - (offset+len) > -fReadAheadSize) &&
			 (fReadAheadSize > 0) ) {

			kXR_int64 araoffset;
			kXR_int32 aralen;

			// This is a HIT case. Async readahead will try to put some data
			// in advance into the cache. The higher the araoffset will be,
			// the best chances we have not to cause overhead
			araoffset = xrdmax(fReadAheadLast, offset + len);
			aralen = xrdmin(fReadAheadSize,
					offset + len + fReadAheadSize -
					xrdmax(offset + len, fReadAheadLast));

			if (aralen > 0) {
			    TrimReadRequest(araoffset, aralen, fReadAheadSize);
			    fReadAheadLast = araoffset + aralen;
			    Read_Async(araoffset, aralen);
			}
		    }

		    fReadWaitData->UnLock();
		    return len;
		}



	

		// We are here if the cache did not give all the data to us
		// We should have a list of blocks to request
		for (int i = 0; i < cacheholes.GetSize(); i++) {
		    kXR_int64 offs;
		    kXR_int32 len;
	    
		    offs = cacheholes[i].beginoffs;
		    len = cacheholes[i].endoffs - offs + 1;


		    Info( XrdClientDebug::kHIDEBUG, "Read",
			  "Hole in the cache: offs=" << offs <<
			  ", len=" << len );
	    
		    Read_Async(offs, len);
		}
	
	
		// Here we forget to have read in advance if the last byte taken is
		// too much before the first read ahead byte
		// if ( fReadAheadLast - 2*fReadAheadSize > (offset+len) ) fReadAheadLast = offset+len-1;

		// Are we using read ahead?
		// We read ahead only if (offs+len) lies in an interval of fReadAheadLast not bigger than the readahead size
		// in advance. But not too much over.
		if ( (fReadAheadLast - (offset+len) < fReadAheadSize) &&
		     (fReadAheadLast - (offset+len) > -fReadAheadSize) &&
		     (fReadAheadSize > 0) ) {

		    kXR_int64 araoffset;
		    kXR_int32 aralen;

		    // This is a HIT case. Async readahead will try to put some data
		    // in advance into the cache. The higher the araoffset will be,
		    // the best chances we have not to cause overhead
                    //if (!bytesgot && !blkstowait && !cacheholes.GetSize()) {
		    //  araoffset = xrdmax(fReadAheadLast, offset);
                    //  blkstowait++;
                    //}
                    //else
		     araoffset = xrdmax(fReadAheadLast, offset + len);

		    aralen = xrdmin(fReadAheadSize,
				    offset + len + fReadAheadSize -
				    xrdmax(offset + len, fReadAheadLast));

		    if (aralen > 0) {
			TrimReadRequest(araoffset, aralen, fReadAheadSize);
			fReadAheadLast = araoffset + aralen;
			Read_Async(araoffset, aralen);
		    }
		}

	    }

	    // If we got nothing from the cache let's do it sync and exit!
	    // Note that this part has the side effect of triggering the recovery actions
	    //  if we get here after an error (or timeout)
	    // Hence it's not a good idea to make async also this read
	    // Remember also that a sync read request must not be modified if it's going to be
	    //  written into the application-given buffer
	    if (retrysync || (!bytesgot && !blkstowait && !cacheholes.GetSize())) {

 	        fReadWaitData->UnLock();

		// We might be here for a suspect comm trouble
		// In this case the outstanding readahead blocks may have been lost
		if (retrysync) fReadAheadLast = offset+len;

		retrysync = false;
                memset(&fConnModule->LastServerError, 0, sizeof(fConnModule->LastServerError));

		Info( XrdClientDebug::kHIDEBUG, "Read",
		      "Read(offs=" << offset <<
		      ", len=" << len << "). Going sync." );

		// Prepare a request header 
		ClientRequest readFileRequest;
		memset( &readFileRequest, 0, sizeof(readFileRequest) );
		fConnModule->SetSID(readFileRequest.header.streamid);
		readFileRequest.read.requestid = kXR_read;
		memcpy( readFileRequest.read.fhandle, fHandle, sizeof(fHandle) );
		readFileRequest.read.offset = offset;
		readFileRequest.read.rlen = len;
		readFileRequest.read.dlen = 0;

		if (!fConnModule->SendGenCommand(&readFileRequest, 0, 0, (void *)buf,
                                                 FALSE, (char *)"ReadBuffer"))
                   return 0;

		return fConnModule->LastServerResp.dlen;
	    }

	    // Now it's time to sleep
	    // This thread will be awakened when new data will arrive
	    if ( (blkstowait > 0) || cacheholes.GetSize() ) {
		Info( XrdClientDebug::kHIDEBUG, "Read",
		      "Waiting " << blkstowait+cacheholes.GetSize() << "outstanding blocks." );

		if (!fConnModule->IsPhyConnConnected() ||
		    fReadWaitData->Wait( EnvGetLong(NAME_REQUESTTIMEOUT) ) ||
                    (fConnModule->LastServerError.errnum != kXR_noErrorYet) ) {

                   fConnModule->LastServerError.errnum = kXR_noErrorYet;

                  if (DebugLevel() >= XrdClientDebug::kUSERDEBUG) {
                    fConnModule->PrintCache();

		    Error( "Read",
                           "Timeout or error waiting outstanding blocks. "
                           "Retrying sync! "
                           "List of outstanding reqs follows." );
                    ConnectionManager->SidManager()->PrintoutOutstandingRequests();
                  }

		    retrysync = true;
		}

		

	    }
	
	    fReadWaitData->UnLock();

	} while ((blkstowait > 0) || cacheholes.GetSize());

    // To lower caching overhead in copy-like applications
    if (EnvGetLong(NAME_REMUSEDCACHEBLKS))
       fConnModule->RemoveDataFromCache(0, offset);

    return len;
}

//_____________________________________________________________________________
kXR_int64 XrdClient::ReadV(char *buf, kXR_int64 *offsets, int *lens, int nbuf)
{
    // If buf==0 then the request is considered as asynchronous

    if (!IsOpen_wait()) {
       Error("ReadV", "File not opened.");
       return 0;
    }

    // This means problems in getting the protocol version
    if ( fConnModule->GetServerProtocol() < 0 ) {
       Info(XrdClientDebug::kHIDEBUG, "ReadV",
            "Problems retrieving protocol version run by the server" );
       return -1;
    }

    // This means the server won't support it
    if ( fConnModule->GetServerProtocol() < 0x00000247 ) {
       Info(XrdClientDebug::kHIDEBUG, "ReadV",
            "The server is an old version " << fConnModule->GetServerProtocol() <<
	    " and doesn't support vectored reading" );
       return -1;
    }

    Stat(0);

    // We pre-process the request list in order to make it compliant
    //  with the restrictions imposed by the server
    XrdClientVector<XrdClientReadVinfo> reqvect(nbuf);

    // First we want to know how much data we expect
    kXR_int64 maxbytes = 0;
    for (int ii = 0; ii < nbuf; ii++)
      maxbytes += lens[ii];

    // Then we get a suggestion about the splitsize to use
    int spltsize = 0;
    int reqsperstream = 0;
    XrdClientMStream::GetGoodSplitParameters(fConnModule, spltsize, reqsperstream, maxbytes);

    // To optimize the splitting for the readv, we need the greater multiple
    // of READV_MAXCHUNKSIZE... maybe yes, maybe not...
    // if (spltsize > 2*READV_MAXCHUNKSIZE) blah blah

    // Each subchunk must not exceed spltsize bytes
    for (int ii = 0; ii < nbuf; ii++)
      XrdClientReadV::PreProcessChunkRequest(reqvect, offsets[ii], lens[ii],
					     fStatInfo.size,
					     spltsize );


    int i = 0, startitem = 0;
    kXR_int64 res = 0, bytesread = 0;

    while ( i < reqvect.GetSize() ) {

      // Here we have the sequence of fixed blocks to request
      // We want to request single readv reqs which
      //  - are compliant with the max number of blocks the server supports
      //  - do not request more than maxbytes bytes each
      kXR_int64 tmpbytes = 0;

      int maxchunkcnt = READV_MAXCHUNKS;
      if (EnvGetLong(NAME_MULTISTREAMCNT) > 0)
         maxchunkcnt = reqvect.GetSize() / EnvGetLong(NAME_MULTISTREAMCNT)+1;

      if (maxchunkcnt < 2) maxchunkcnt = 2;
      if (maxchunkcnt > READV_MAXCHUNKS) maxchunkcnt = READV_MAXCHUNKS;

      int chunkcnt = 0;
      while ( i < reqvect.GetSize() ) {
	if (chunkcnt >= maxchunkcnt) break;
	if (tmpbytes + reqvect[i].len > spltsize) break;
	tmpbytes += reqvect[i].len;
	chunkcnt++;
	i++;
      }


      if (i-startitem == 1) {
         if (buf) {
            // Synchronous
            res = Read(buf, reqvect[startitem].offset, reqvect[startitem].len);
            
         } else {
            // Asynchronous, res stays the same
            Read_Async(reqvect[startitem].offset, reqvect[startitem].len);
         }
      } else {
         if (buf)
            res = XrdClientReadV::ReqReadV(fConnModule, fHandle, buf+bytesread,
                                           reqvect, startitem, i-startitem,
                                           fConnModule->GetParallelStreamToUse(reqsperstream) );
         else
            res = XrdClientReadV::ReqReadV(fConnModule, fHandle, 0,
                                           reqvect, startitem, i-startitem,
                                           fConnModule->GetParallelStreamToUse(reqsperstream) );
      }

      // The next bunch of chunks to request starts from here
      startitem = i;

      if ( res < 0 )
	break;

      bytesread += res;

    }
    
    // pos will indicate the size of the data read
    // Even if we were able to read only a part of the buffer !!!
    return bytesread;
}


//_____________________________________________________________________________
bool XrdClient::Write(const void *buf, long long offset, int len) {

    if (!IsOpen_wait()) {
	Error("WriteBuffer", "File not opened.");
	return FALSE;
    }


    // Prepare request
    ClientRequest writeFileRequest;
    memset( &writeFileRequest, 0, sizeof(writeFileRequest) );
    fConnModule->SetSID(writeFileRequest.header.streamid);
    writeFileRequest.write.requestid = kXR_write;
    memcpy( writeFileRequest.write.fhandle, fHandle, sizeof(fHandle) );

    bool ret = false;

    if (!fUseCache) {
      // Silly situation but worth handling
      writeFileRequest.write.pathid = 0;
      writeFileRequest.write.dlen = len;
      writeFileRequest.write.offset = offset;
      ret = fConnModule->SendGenCommand(&writeFileRequest, (void *)buf, 0, 0,
					FALSE, (char *)"Write");

      if (ret && fStatInfo.stated)
         fStatInfo.size = xrdmax(fStatInfo.size, offset + len);

      return ret;
    }

    // Soft checkpoint, we check just for timeouts in old outstanding write requests
    // An unrecoverable error in an old request gives no sense to continue here.
    // Rather unfortunate but happens. One more weird metaphor of life?!?!?
    if (!fConnModule->DoWriteSoftCheckPoint()) return false;

    fConnModule->RemoveDataFromCache(offset, offset+len-1, true);

    XrdClientVector<XrdClientMStream::ReadChunk> rl;
    XrdClientMStream::SplitReadRequest(fConnModule, offset, len, rl);
    kXR_char *cbuf = (kXR_char *)buf;
    int writtenok = 0;

    for (int i = 0; i < rl.GetSize(); i++) {

      writeFileRequest.write.offset = rl[i].offset;
      writeFileRequest.write.dlen = rl[i].len;
      writeFileRequest.write.pathid = rl[i].streamtosend;
   
      // The req is sent only asynchronously. So, the only bottleneck here is the kernel
      // and its tcp buffer sizes... and the network of course. But beware of the crappy
      // default tcp settings of the various SLCs
      XReqErrorType b;
      int cnt = 0;
      do {
	b = fConnModule->WriteToServer_Async(&writeFileRequest, (kXR_char *)buf+(rl[i].offset-offset), rl[i].streamtosend);
	ret = (b == kOK);
	if (b != kNOMORESTREAMS) break;

	// There are no more slots for outstanding requests
	// Asking for a hard checkpoint is a good way to waste some time
	// and to wait for some slots to be free
	// The only drawback is that the mechanism needs enough memory to fill
	// the pipeline given by the network+server latency, or the max number of available slots
	if (!fConnModule->DoWriteHardCheckPoint()) break;
      } while (cnt < 10);

      if (b != kOK) {
	// We need to deal with errors while sending the request
	// So, if there is an error or timeout while sending the req, it has to be retried sync
	// in order to trigger immediately the normal retry mechanism, if needed
	// Try again the write op, but in sync mode
	writeFileRequest.write.pathid = 0;
	ret = fConnModule->SendGenCommand(&writeFileRequest, (kXR_char *)buf+(rl[i].offset-offset), 0, 0,
					  FALSE, (char *)"Write");
	if (!ret) break;
      }
      writtenok += rl[i].len;
      cbuf += rl[i].len;
    }

    if (ret && fStatInfo.stated)
       fStatInfo.size = xrdmax(fStatInfo.size, offset + writtenok);

    return ret;
}


//_____________________________________________________________________________
bool XrdClient::Sync()
{
    // Flushes un-written data

 
    if (!IsOpen_wait()) {
	Error("Sync", "File not opened.");
	return FALSE;
    }

    if (!fConnModule->DoWriteHardCheckPoint()) return false;

    // Prepare request
    ClientRequest flushFileRequest;
    memset( &flushFileRequest, 0, sizeof(flushFileRequest) );

    fConnModule->SetSID(flushFileRequest.header.streamid);

    flushFileRequest.sync.requestid = kXR_sync;

    memcpy(flushFileRequest.sync.fhandle, fHandle, sizeof(fHandle));

    flushFileRequest.sync.dlen = 0;

    return fConnModule->SendGenCommand(&flushFileRequest, 0, 0, 0, 
                                       FALSE, (char *)"Sync");
  
}

//_____________________________________________________________________________
bool XrdClient::TryOpen(kXR_unt16 mode, kXR_unt16 options, bool doitparallel) {
   
    int thrst = 0;

    fOpenPars.inprogress = true;

    if (doitparallel) {

	for (int i = 0; i < DFLT_MAXCONCURRENTOPENS; i++) {

	    fConcOpenSem.Wait();
	    fOpenerTh = new XrdClientThread(FileOpenerThread);

	    thrst = fOpenerTh->Run(this);     
	    if (!thrst) {
		// The thread start seems OK. This open will go in parallel

		//if (fOpenerTh->Detach())
		//    Error("XrdClient", "Thread detach failed. Low system resources?");

		return true;
	    }

	    // Note: the Post() here is intentionally missing.

	    Error("XrdClient", "Parallel open thread start failed. Low system"
		  " resources? Res=" << thrst << " Count=" << i);
	    delete fOpenerTh;
	    fOpenerTh = 0;

	}

	// If we are here it seems that this machine cannot start open threads at all
	// In this desperate situation we try to go sync anyway.
	for (int i = 0; i < DFLT_MAXCONCURRENTOPENS; i++) fConcOpenSem.Post();

	Error("XrdClient", "All the parallel open thread start attempts failed."
	      " Desperate situation. Going sync.");
     
	doitparallel = false;
    }

    // First attempt to open a remote file
    bool lowopenRes = LowOpen(fUrl.File.c_str(), mode, options);
    if (lowopenRes) {

	// And here we fire up the needed parallel streams
	XrdClientMStream::EstablishParallelStreams(fConnModule);
        int retc;

        if (!fConnModule->IsConnected()) {
           fOpenPars.opened = false;
           retc = false;
        } else retc = true;
        
        TerminateOpenAttempt();
        return retc; 

    }

    // If the open request failed for the error "file not found" proceed, 
    // otherwise return FALSE
    if (fConnModule->LastServerResp.status != kXR_NotFound) {
	TerminateOpenAttempt();
	return FALSE;
    }


    // If connected to a host saying "File not Found" or similar then...

    // If we are currently connected to a host which is different
    // from the one we formerly connected, then we resend the request
    // specifyng the supposed failing server as opaque info
    if (fConnModule->GetLBSUrl() &&
	(fConnModule->GetCurrentUrl().Host != fConnModule->GetLBSUrl()->Host) ) {
	XrdOucString opinfo;

	opinfo = "&tried=" + fConnModule->GetCurrentUrl().Host;

	Info(XrdClientDebug::kUSERDEBUG,
	     "Open", "Back to " << fConnModule->GetLBSUrl()->Host <<
	     ". Refreshing cache. Opaque info: " << opinfo);

        // First disconnect the current logical connection (otherwise spurious
        // connection will stay around and create problems with processing of
        // unsolicited messages)
        fConnModule->Disconnect(FALSE);

	if ( (fConnModule->GoToAnotherServer(*fConnModule->GetLBSUrl()) == kOK) &&
	     LowOpen(fUrl.File.c_str(), mode, options | kXR_refresh,
		     (char *)opinfo.c_str() ) ) {

	    // And here we fire up the needed parallel streams
	    XrdClientMStream::EstablishParallelStreams(fConnModule);

	    TerminateOpenAttempt();
	    return TRUE;
	}
	else {

	    Error("Open", "Error opening the file.");
	    TerminateOpenAttempt();
	    return FALSE;
	}

    }

    TerminateOpenAttempt();
    return FALSE;

}

//_____________________________________________________________________________
bool XrdClient::LowOpen(const char *file, kXR_unt16 mode, kXR_unt16 options,
			char *additionalquery) {

    // Low level Open method
    XrdOucString finalfilename(file);

    if ((fConnModule->fRedirOpaque.length() > 0) || additionalquery) {
	finalfilename += "?";

	if (fConnModule->fRedirOpaque.length() > 0)
	  finalfilename += fConnModule->fRedirOpaque;

	if (additionalquery)
	  finalfilename += additionalquery;
    }





    // Send a kXR_open request in order to open the remote file
    ClientRequest openFileRequest;

    char buf[1024];
    struct ServerResponseBody_Open *openresp = (struct ServerResponseBody_Open *)buf;

    memset(&openFileRequest, 0, sizeof(openFileRequest));

    fConnModule->SetSID(openFileRequest.header.streamid);

    openFileRequest.header.requestid = kXR_open;

    openFileRequest.open.options = options | kXR_retstat;

    // Set the open mode field
    openFileRequest.open.mode = mode;

    // Set the length of the data (in this case data describes the path and 
    // file name)
    openFileRequest.open.dlen = finalfilename.length();

    // Send request to server and receive response
    bool resp = fConnModule->SendGenCommand(&openFileRequest,
					    (const void *)finalfilename.c_str(),
					    0, openresp, false, (char *)"Open");

    if (resp && (fConnModule->LastServerResp.status == 0)) {
       // Get the file handle to use for future read/write...
       if (fConnModule->LastServerResp.dlen >= (kXR_int32)sizeof(fHandle)) {

          memcpy( fHandle, openresp->fhandle, sizeof(fHandle) );

          fOpenPars.opened = TRUE;
          fOpenPars.options = options;
          fOpenPars.mode = mode;
       }
       else
          Error("Open",
                "Server did not return a filehandle. Protocol error.");

       if (fConnModule->LastServerResp.dlen > 12) {
	  // Get the stats
	  Info(XrdClientDebug::kHIDEBUG,
	       "Open", "Returned stats=" << ((char *)openresp + sizeof(struct ServerResponseBody_Open)));
          
	  sscanf((char *)openresp + sizeof(struct ServerResponseBody_Open), "%ld %lld %ld %ld",
		 &fStatInfo.id,
		 &fStatInfo.size,
		 &fStatInfo.flags,
		 &fStatInfo.modtime);
          
	  fStatInfo.stated = true;
       }
       
    }


    return fOpenPars.opened;
}

//_____________________________________________________________________________
bool XrdClient::Stat(struct XrdClientStatInfo *stinfo, bool force) {

    if (!force && fStatInfo.stated) {
	if (stinfo)
	    memcpy(stinfo, &fStatInfo, sizeof(fStatInfo));
	return TRUE;
    }

    if (!IsOpen_wait()) {
       Error("Stat", "File not opened.");
       return FALSE;
    }

    if (force && !Sync()) return false;

    // asks the server for stat file informations
    ClientRequest statFileRequest;
   
    memset(&statFileRequest, 0, sizeof(ClientRequest));
   
    fConnModule->SetSID(statFileRequest.header.streamid);
   
    statFileRequest.stat.requestid = kXR_stat;
    memset(statFileRequest.stat.reserved, 0, 
	   sizeof(statFileRequest.stat.reserved));

    statFileRequest.stat.dlen = fUrl.File.length();
   
    char fStats[2048];
    memset(fStats, 0, 2048);

    bool ok = fConnModule->SendGenCommand(&statFileRequest,
					  (const char*)fUrl.File.c_str(),
					  0, fStats , FALSE, (char *)"Stat");

    if (ok && (fConnModule->LastServerResp.status == 0) ) {

	Info(XrdClientDebug::kHIDEBUG,
	     "Stat", "Returned stats=" << fStats);
   
	sscanf(fStats, "%ld %lld %ld %ld",
	       &fStatInfo.id,
	       &fStatInfo.size,
	       &fStatInfo.flags,
	       &fStatInfo.modtime);

	if (stinfo)
	    memcpy(stinfo, &fStatInfo, sizeof(fStatInfo));

	fStatInfo.stated = true;
    }

    return ok;
}

//_____________________________________________________________________________
bool XrdClient::Close() {

    if (!IsOpen_wait()) {
	Info(XrdClientDebug::kUSERDEBUG, "Close", "File not opened.");
	return TRUE;
    }

    ClientRequest closeFileRequest;
  
    memset(&closeFileRequest, 0, sizeof(closeFileRequest) );

    fConnModule->SetSID(closeFileRequest.header.streamid);

    closeFileRequest.close.requestid = kXR_close;
    memcpy(closeFileRequest.close.fhandle, fHandle, sizeof(fHandle) );
    closeFileRequest.close.dlen = 0;

    // Use the sync one only if the file was opened for writing
    // To enforce the server side correct data flushing
    if (IsOpenedForWrite())
      fConnModule->DoWriteHardCheckPoint();

    fConnModule->SendGenCommand(&closeFileRequest,
				0,
				0, 0 , FALSE, (char *)"Close");
  
    // No file is opened for now
    fOpenPars.opened = FALSE;

    return TRUE;
}


//_____________________________________________________________________________
bool XrdClient::OpenFileWhenRedirected(char *newfhandle, bool &wasopen)
{
    // Called by the comm module when it needs to reopen a file
    // after a redir

    wasopen = fOpenPars.opened;

    if (!fOpenPars.opened)
	return TRUE;

    fOpenPars.opened = FALSE;

    Info(XrdClientDebug::kHIDEBUG,
	 "OpenFileWhenRedirected", "Trying to reopen the same file." );

    kXR_unt16 options = fOpenPars.options;

    if (fOpenPars.options & kXR_delete) {
	Info(XrdClientDebug::kHIDEBUG,
	     "OpenFileWhenRedirected", "Stripping off the 'delete' option." );

	options &= !kXR_delete;
	options |= kXR_open_updt;
    }

    if (fOpenPars.options & kXR_new) {
	Info(XrdClientDebug::kHIDEBUG,
	     "OpenFileWhenRedirected", "Stripping off the 'new' option." );

	options &= !kXR_new;
	options |= kXR_open_updt;
    }

    if ( TryOpen(fOpenPars.mode, options, false) ) {

	fOpenPars.opened = TRUE;

	Info(XrdClientDebug::kHIDEBUG,
	     "OpenFileWhenRedirected",
	     "Open successful." );

	memcpy(newfhandle, fHandle, sizeof(fHandle));

	return TRUE;
    } else {
	Error("OpenFileWhenRedirected", 
	      "File open failed.");
      
	return FALSE;
    }
}

//_____________________________________________________________________________
bool XrdClient::Copy(const char *localpath) {

    if (!IsOpen_wait()) {
	Error("Copy", "File not opened.");
	return FALSE;
    }

    Stat(0);
    int f = open(localpath, O_CREAT | O_RDWR, 0);   
    if (f < 0) {
	Error("Copy", "Error opening local file.");
	return FALSE;
    }

    void *buf = malloc(100000);
    long long offs = 0;
    int nr = 1;

    while ((nr > 0) && (offs < fStatInfo.size))
	if ( (nr = Read(buf, offs, 100000)) )
	    offs += write(f, buf, nr);
	 
    close(f);
    free(buf);
   
    return TRUE;
}

//_____________________________________________________________________________
UnsolRespProcResult XrdClient::ProcessUnsolicitedMsg(XrdClientUnsolMsgSender *sender,
						     XrdClientMessage *unsolmsg) {
    // We are here if an unsolicited response comes from a logical conn
    // The response comes in the form of a TXMessage *, that must NOT be
    // destroyed after processing. It is destroyed by the first sender.
    // Remember that we are in a separate thread, since unsolicited 
    // responses are asynchronous by nature.

    if ( unsolmsg->GetStatusCode() != XrdClientMessage::kXrdMSC_ok ) {
      Info(XrdClientDebug::kHIDEBUG,
	   "ProcessUnsolicitedMsg", "Incoming unsolicited communication error message." );
    }
    else {
	Info(XrdClientDebug::kHIDEBUG,
	     "ProcessUnsolicitedMsg", "Incoming unsolicited response from streamid " <<
	     unsolmsg->HeaderSID() );
    }

    // Local processing ....

    if (unsolmsg->IsAttn()) {
	struct ServerResponseBody_Attn *attnbody;

	attnbody = (struct ServerResponseBody_Attn *)unsolmsg->GetData();

        int actnum = (attnbody) ? (attnbody->actnum) : 0;

	// "True" async resp is processed here
	switch (actnum) {

	case kXR_asyncdi:
	    // Disconnection + delayed reconnection request

	    struct ServerResponseBody_Attn_asyncdi *di;
	    di = (struct ServerResponseBody_Attn_asyncdi *)unsolmsg->GetData();

	    // Explicit redirection request
	    if (di) {
		Info(XrdClientDebug::kUSERDEBUG,
		     "ProcessUnsolicitedMsg", "Requested Disconnection + Reconnect in " <<
		     ntohl(di->wsec) << " seconds.");

		fConnModule->SetRequestedDestHost((char *)fUrl.Host.c_str(), fUrl.Port);
		fConnModule->SetREQDelayedConnectState(ntohl(di->wsec));
	    }

	    // Other objects may be interested in this async resp
	    return kUNSOL_CONTINUE;
	    break;
	 
	case kXR_asyncrd:
	    // Redirection request

	    struct ServerResponseBody_Attn_asyncrd *rd;
	    rd = (struct ServerResponseBody_Attn_asyncrd *)unsolmsg->GetData();

	    // Explicit redirection request
	    if (rd && (strlen(rd->host) > 0)) {
		Info(XrdClientDebug::kUSERDEBUG,
		     "ProcessUnsolicitedMsg", "Requested redir to " << rd->host <<
		     ":" << ntohl(rd->port));

		fConnModule->SetRequestedDestHost(rd->host, ntohl(rd->port));
	    }

	    // Other objects may be interested in this async resp
	    return kUNSOL_CONTINUE;
	    break;

	case kXR_asyncwt:
	    // Puts the client in wait state

	    struct ServerResponseBody_Attn_asyncwt *wt;
	    wt = (struct ServerResponseBody_Attn_asyncwt *)unsolmsg->GetData();

	    if (wt) {
		Info(XrdClientDebug::kUSERDEBUG,
		     "ProcessUnsolicitedMsg", "Pausing client for " << ntohl(wt->wsec) <<
		     " seconds.");

		fConnModule->SetREQPauseState(ntohl(wt->wsec));
	    }

	    // Other objects may be interested in this async resp
	    return kUNSOL_CONTINUE;
	    break;

	case kXR_asyncgo:
	    // Resumes from pause state

	    Info(XrdClientDebug::kUSERDEBUG,
		 "ProcessUnsolicitedMsg", "Resuming from pause.");

	    fConnModule->SetREQPauseState(0);

	    // Other objects may be interested in this async resp
	    return kUNSOL_CONTINUE;
	    break;

	case kXR_asynresp:
	    // A response to a request which got a kXR_waitresp as a response
	
	    // We pass it direcly to the connmodule for processing
	    // The processing will tell if the streamid matched or not,
	    // in order to stop further processing
	    return fConnModule->ProcessAsynResp(unsolmsg);
	    break;

	default:

	    Info(XrdClientDebug::kUSERDEBUG,
	         "ProcessUnsolicitedMsg", "Empty message");

	    // Propagate the message
	    return kUNSOL_CONTINUE;

	} // switch

      
    }
    else
	// Let's see if the message is a communication error message
       if (unsolmsg->GetStatusCode() != XrdClientMessage::kXrdMSC_ok){
          // This is a low level error. The outstanding things have to be terminated
          // Awaken all the waiting threads, some of them may be interested
          fReadWaitData->Broadcast();
          TerminateOpenAttempt();
	 
	  return fConnModule->ProcessAsynResp(unsolmsg);
	}
	else
	    // Let's see if we are receiving the response to an async read/write request
	    if ( ConnectionManager->SidManager()->JoinedSids(fConnModule->GetStreamID(),
                                                             unsolmsg->HeaderSID()) ) {
		struct SidInfo *si =
                   ConnectionManager->SidManager()->GetSidInfo(unsolmsg->HeaderSID());

                if (!si) {
                   Error("ProcessUnsolicitedMsg",
                         "Orphaned streamid detected: " << unsolmsg->HeaderSID());
                   return kUNSOL_DISPOSE;
                }


		ClientRequest *req = &(si->outstandingreq);
	 
		Info(XrdClientDebug::kHIDEBUG,
		     "ProcessUnsolicitedMsg",
		     "Processing async response from streamid " <<
		     unsolmsg->HeaderSID() << " father=" <<
		     si->fathersid );
                
		// We are interested in data, not errors here...
		if ( (unsolmsg->HeaderStatus() == kXR_oksofar) || 
		     (unsolmsg->HeaderStatus() == kXR_ok) ) {

		    switch (req->header.requestid) {

		    case kXR_read: {
			long long offs = req->read.offset + si->reqbyteprogress;
	    
			Info(XrdClientDebug::kHIDEBUG, "ProcessUnsolicitedMsg",
			     "Putting kXR_read data into cache. Offset=" <<
			     offs <<
			     " len " <<
			     unsolmsg->fHdr.dlen);

			{
			// Keep in sync with the cache lookup
			XrdSysCondVarHelper cndh(fReadWaitData);

			// To compute the end offset of the block we have to take 1 from the size!
			fConnModule->SubmitDataToCache(unsolmsg, offs,
						       offs + unsolmsg->fHdr.dlen - 1);

			}
			si->reqbyteprogress += unsolmsg->fHdr.dlen;

			// Awaken all the waiting threads, some of them may be interested
			fReadWaitData->Broadcast();

			if (unsolmsg->HeaderStatus() == kXR_ok) return kUNSOL_DISPOSE;
			else return kUNSOL_KEEP;

			break;
		    }

		    case kXR_readv: {
	    
			Info(XrdClientDebug::kHIDEBUG, "ProcessUnsolicitedMsg",
			     "Putting kXR_readV data into cache. " <<
			     " len " <<
			     unsolmsg->fHdr.dlen);
			{
			// Keep in sync with the cache lookup
			XrdSysCondVarHelper cndh(fReadWaitData);

			XrdClientReadV::SubmitToCacheReadVResp(fConnModule, (char *)unsolmsg->DonateData(),
							       unsolmsg->fHdr.dlen);
			}
			// Awaken all the sleepers. Some of them may be interested
			fReadWaitData->Broadcast();

			if (unsolmsg->HeaderStatus() == kXR_ok) return kUNSOL_DISPOSE;
			else return kUNSOL_KEEP;

			break;
		    }
		      

		    case kXR_write: {
		      Info(XrdClientDebug::kHIDEBUG, "ProcessUnsolicitedMsg",
			   "Got positive ack for write req " << req->header.dlen <<
			   "@" << req->write.offset);

			// the corresponding cache blk has to be unpinned, and eventually
			// purged
			fConnModule->UnPinCacheBlk(req->write.offset, req->write.offset+req->header.dlen);

                        // A bit cpu consuming... need to optimize this
                        if (EnvGetLong(NAME_PURGEWRITTENBLOCKS))
                           fConnModule->RemoveDataFromCache(req->write.offset, req->write.offset+req->header.dlen-1, true);

		      // This streamid will be released
		      return kUNSOL_DISPOSE;
		    }
		    }
		    
		} // if oksofar or ok
                else {
                   // And here we treat the errors which can be fatal or just ugly to ignore
                   //  even if the strategy should be completely async
                    switch (req->header.requestid) {

		    case kXR_read: {
                        // We invalidate the whole request in the cache
	    
			Error("ProcessUnsolicitedMsg",
			     "Got a kxr_read error. Req offset=" <<
			     req->read.offset <<
			     " len=" <<
			     req->read.rlen);

			{
			// Keep in sync with the cache lookup
			XrdSysCondVarHelper cndh(fReadWaitData);

			// To compute the end offset of the block we have to take 1 from the size!
                        // Note that this is an error, we try to invalidate everythign which
                        // can be related to this chunk
			fConnModule->RemoveDataFromCache(req->read.offset,
                                                         req->read.offset + req->read.rlen - 1, true);

			}


                        // Print out the error information, as received by the server	
                        struct ServerResponseBody_Error *body_err;
                        body_err = (struct ServerResponseBody_Error *)(unsolmsg->GetData());
                        if (body_err)
                           Info(XrdClientDebug::kNODEBUG, "ProcessUnsolicitedMsg", "Server declared: " <<
                                (const char*)body_err->errmsg << "(error code: " << ntohl(body_err->errnum) << ")");
                        
                        // Save the last error received
                        memset(&fConnModule->LastServerError, 0, sizeof(fConnModule->LastServerError));
                        memcpy(&fConnModule->LastServerError, body_err,
                               xrdmin(sizeof(fConnModule->LastServerError), (unsigned)unsolmsg->DataLen()) );
                        fConnModule->LastServerError.errnum = ntohl(body_err->errnum);
	

			// Awaken all the waiting threads, some of them may be interested
			fReadWaitData->Broadcast();

			// Other clients might be interested
			return kUNSOL_CONTINUE;

			break;
		    }
                    case kXR_write: {
                       Error("ProcessUnsolicitedMsg",
                              "Got a kxr_write error. Req offset=" <<
                             req->write.offset <<
                             " len=" <<
                             req->write.dlen);
                     
  
                       // Print out the error information, as received by the server	
                       struct ServerResponseBody_Error *body_err;
                       body_err = (struct ServerResponseBody_Error *)(unsolmsg->GetData());
                       if (body_err) {
                          Info(XrdClientDebug::kNODEBUG, "ProcessUnsolicitedMsg", "Server declared: " <<
                               (const char*)body_err->errmsg << "(error code: " << ntohl(body_err->errnum) << ") writing " <<
                               req->write.dlen << "@" << req->write.offset);
                       
                          // Save the last error received
                          memset(&fConnModule->LastServerError, 0, sizeof(fConnModule->LastServerError));
                          memcpy(&fConnModule->LastServerError, body_err,
                                 xrdmin(sizeof(fConnModule->LastServerError), (unsigned)unsolmsg->DataLen()) );
                          fConnModule->LastServerError.errnum = ntohl(body_err->errnum);
                       
                          // Mark the request as an error. It will be catched by the next write soft checkpoint
                          ConnectionManager->SidManager()->ReportSidResp(unsolmsg->HeaderSID(),
                                                                         unsolmsg->GetStatusCode(),
                                                                         ntohl(body_err->errnum),
                                                                         body_err->errmsg);
                       }
                       else
                          ConnectionManager->SidManager()->ReportSidResp(unsolmsg->HeaderSID(),
                                                                         unsolmsg->GetStatusCode(),
                                                                         kXR_noErrorYet,
                                                                         0);

                       // Awaken all the waiting threads, some of them may be interested
                       fReadWaitData->Broadcast();
                       
                       // This streamid must be kept as pending. It will be handled by the subsequent
                       // write checkpoint
                       return kUNSOL_KEEP;

                       break;
                    }

                    } // switch
                } // else




			 
	 
	    }
   
   
    return kUNSOL_CONTINUE;
}

XReqErrorType XrdClient::Read_Async(long long offset, int len) {

    if (!IsOpen_wait()) {
	Error("Read", "File not opened.");
	return kGENERICERR;
    }

    Stat(0);
    len = xrdmin(fStatInfo.size - offset, len);
 
    if (len <= 0) return kOK;

    if (fUseCache)
	fConnModule->SubmitPlaceholderToCache(offset, offset+len-1);
    else return kOK;

    // Prepare request
    ClientRequest readFileRequest;
    memset( &readFileRequest, 0, sizeof(readFileRequest) );

    // No need to initialize the streamid, it will be filled by XrdClientConn
    readFileRequest.read.requestid = kXR_read;
    memcpy( readFileRequest.read.fhandle, fHandle, sizeof(fHandle) );
    readFileRequest.read.offset = offset;
    readFileRequest.read.rlen = len;
    readFileRequest.read.dlen = 0;

    Info(XrdClientDebug::kHIDEBUG, "Read_Async",
	 "Requesting to read " <<
	 readFileRequest.read.rlen <<
	 " bytes of data at offset " <<
	 readFileRequest.read.offset);
    
    // This request might be splitted and distributed through multiple streams
    XrdClientVector<XrdClientMStream::ReadChunk> chunks;
    XReqErrorType ok = kOK;

    if (XrdClientMStream::SplitReadRequest(fConnModule, offset, len,
					   chunks) ) {

	for (int i = 0; i < chunks.GetSize(); i++) {
	    XrdClientMStream::ReadChunk *c;

	    read_args args;
	    memset(&args, 0, sizeof(args));

	    c = &chunks[i];
	    args.pathid = c->streamtosend;
	    
	    Info(XrdClientDebug::kHIDEBUG, "Read_Async",
		 "Requesting pathid " << c->streamtosend);
	    
	    readFileRequest.read.offset = c->offset;
	    readFileRequest.read.rlen = c->len;

	    if (args.pathid != 0) {
	      readFileRequest.read.dlen = sizeof(read_args);
	      ok = fConnModule->WriteToServer_Async(&readFileRequest, &args,
						    0);
	    }
	    else {
	      readFileRequest.read.dlen = 0;
	      ok = fConnModule->WriteToServer_Async(&readFileRequest, 0,
						    0);
	    }


	    if (ok != kOK) break;
	}
    }
    else
	return (fConnModule->WriteToServer_Async(&readFileRequest, 0));

    return ok;

}


bool XrdClient::TrimReadRequest(kXR_int64 &offs, kXR_int32 &len, kXR_int32 rasize) {

    kXR_int64 newoffs;
    kXR_int32 newlen, blksz;

    if (!fUseCache ) return false;

//    blksz = xrdmax(rasize, 16384);

//     kXR_int64 minlen;
//     newoffs = offs / blksz * blksz;
//     minlen = (offs + len - newoffs);
//     newlen = ((minlen / blksz + 1) * blksz);
//     newlen = xrdmax(rasize, newlen);

    blksz = 128*1024;
    kXR_int64 lastbyte;
    newoffs = offs;
    lastbyte = offs+len+blksz-1;
    lastbyte = (lastbyte / blksz) * blksz - 1;
    
    newlen = lastbyte-newoffs+1;

    if (fConnModule->CacheWillFit(newlen)) {
	offs = newoffs;
	len = newlen;
	return true;
    }

    return false;

}

//_____________________________________________________________________________
// Truncates the open file at a specified length
bool XrdClient::Truncate(long long len) {

    if (!IsOpen_wait()) {
	Info(XrdClientDebug::kUSERDEBUG, "Truncate", "File not opened.");
	return true;
    }

    ClientRequest truncFileRequest;
  
    memset(&truncFileRequest, 0, sizeof(truncFileRequest) );

    fConnModule->SetSID(truncFileRequest.header.streamid);

    truncFileRequest.truncate.requestid = kXR_truncate;
    memcpy(truncFileRequest.truncate.fhandle, fHandle, sizeof(fHandle) );
    truncFileRequest.truncate.offset = len;

    bool ok = fConnModule->SendGenCommand(&truncFileRequest,
                                          0,
                                          0, 0 , FALSE, (char *)"Truncate");

    if (ok && fStatInfo.stated) fStatInfo.size = len;

    return ok;
}



//_____________________________________________________________________________
// Sleeps on a condvar which is signalled when a new async block arrives
void XrdClient::WaitForNewAsyncData() {
    XrdSysCondVarHelper cndh(fReadWaitData);

    fReadWaitData->Wait();

}

//_____________________________________________________________________________
bool XrdClient::UseCache(bool u)
{
  // Set use of cache flag after checking if the requested value make sense.
  // Returns the previous value to allow quick toggling of the flag.

  bool r = fUseCache;

  if (!u) {
    fUseCache = false;
  } else {
    int size;
    long long bytessubmitted, byteshit, misscount, readreqcnt;
    float missrate, bytesusefulness;


    if ( fConnModule &&
	 fConnModule->GetCacheInfo(size, bytessubmitted, byteshit, misscount, missrate, readreqcnt, bytesusefulness) &&
	 size )
      fUseCache = true;
  }

  // Return the previous setting
  return r;
}

