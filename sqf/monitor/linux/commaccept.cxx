///////////////////////////////////////////////////////////////////////////////
//
// @@@ START COPYRIGHT @@@
//
// (C) Copyright 2009-2014 Hewlett-Packard Development Company, L.P.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
// @@@ END COPYRIGHT @@@
//
///////////////////////////////////////////////////////////////////////////////

using namespace std;

#include "commaccept.h"
#include "monlogging.h"
#include "montrace.h"
#include "monitor.h"

#include <signal.h>

extern CMonitor *Monitor;
extern CNode *MyNode;
extern CNodeContainer *Nodes;
extern char MyPort[MPI_MAX_PORT_NAME];
extern char *ErrorMsg (int error_code);

CCommAccept::CCommAccept(): shutdown_(false), thread_id_(0)
{
    const char method_name[] = "CCommAccept::CCommAccept";
    TRACE_ENTRY;


    TRACE_EXIT;
}

CCommAccept::~CCommAccept()
{
    const char method_name[] = "CCommAccept::~CCommAccept";
    TRACE_ENTRY;


    TRACE_EXIT;
}


struct message_def *CCommAccept::Notice( const char *msgText )
{
    struct message_def *msg;

    const char method_name[] = "CCluster::Notice";
    TRACE_ENTRY;
    
    msg = new struct message_def;
    msg->type = MsgType_ReintegrationError;
    msg->noreply = true;
    msg->u.request.type = ReqType_Notice;
    strncpy( msg->u.request.u.reintegrate.msg, msgText,
             sizeof(msg->u.request.u.reintegrate.msg) );

    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
        trace_printf("%s@%d - Reintegrate notice %s\n",
                     method_name, __LINE__, msgText );

    TRACE_EXIT;

    return msg;
}

// Send node names and port numbers for all existing monitors
// to the new monitor.
bool CCommAccept::sendNodeInfo ( MPI_Comm interComm )
{
    const char method_name[] = "CCommAccept::sendNodeInfo";
    TRACE_ENTRY;
    bool sentData = true;

    int cfgPNodes = Monitor->GetNumNodes();

    nodeId_t *nodeInfo;
    nodeInfo = new nodeId_t[cfgPNodes];
    int rc;

    CNode *node;

    for (int i=0; i<cfgPNodes; ++i)
    {
        node = Nodes->GetNode( i );
        if ( node->GetState() == State_Up)
        {
            strncpy(nodeInfo[i].nodeName, node->GetName(),
                    sizeof(nodeInfo[i].nodeName));
            strncpy(nodeInfo[i].port, node->GetPort(),
                    sizeof(nodeInfo[i].port));

            if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
            {
                trace_printf("%s@%d - Port for node %d (%s): %s\n",
                             method_name, __LINE__, i, node->GetName(),
                             node->GetPort());
            }
        }
        else
        {
            if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
            {
                trace_printf("%s@%d - No port for node %d (node not up)\n",
                             method_name, __LINE__, i);
            }

            nodeInfo[i].nodeName[0] = '\0';
            nodeInfo[i].port[0] = '\0';
        }
    }

    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
    {
       trace_printf("%s@%d - Sending port info to new monitor\n", method_name,
                    __LINE__);
    }

    rc = Monitor->Send((char *) nodeInfo, sizeof(nodeId_t)*cfgPNodes, 0,
                       MON_XCHNG_DATA, interComm);
    if ( rc != MPI_SUCCESS )
    {
        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], cannot send node/port info to "
                 " new monitor process: %s.\n"
                 , method_name, ErrorMsg(rc));
        mon_log_write(MON_COMMACCEPT_4, SQ_LOG_ERR, buf); 

        sentData = false;
    }

    delete [] nodeInfo;

    TRACE_EXIT;

    return sentData;
}

void CCommAccept::processNewComm(MPI_Comm interComm)
{
    const char method_name[] = "CCommAccept::processNewComm";
    TRACE_ENTRY;

    int rc;
    MPI_Comm intraComm;
    nodeId_t nodeId;

    mem_log_write(CMonLog::MON_CONNTONEWMON_2);

    MPI_Comm_set_errhandler( interComm, MPI_ERRORS_RETURN );

    // Get info about connecting monitor
    rc = Monitor->Receive((char *) &nodeId, sizeof(nodeId_t),
                          MPI_ANY_SOURCE, MON_XCHNG_DATA, interComm);
    if ( rc != MPI_SUCCESS )
    {   // Handle error
        MPI_Comm_free( &interComm );

        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], unable to obtain node id from new "
                 "monitor: %s.\n", method_name, ErrorMsg(rc));
        mon_log_write(MON_COMMACCEPT_6, SQ_LOG_ERR, buf);    
        return;
    }

    if ( nodeId.creator )
    {
        // Indicate that this node is the creator monitor for the node up
        // operation.
        MyNode->SetCreator( true, nodeId.creatorShellPid );
    }
    
    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
    {
        trace_printf("%s@%d - Accepted connection from node %s, port=%s, creator=%d, creatorShellPid=%d\n",
                     method_name, __LINE__, nodeId.nodeName, nodeId.port, nodeId.creator, nodeId.creatorShellPid);
    }

    CNode * node= Nodes->GetNode( nodeId.nodeName );
    int pnid = -1;
    if ( node != NULL )
    {   // Store port number for the node
        pnid = node->GetPNid();
        node->SetPort( nodeId.port );
    }
    else
    {
        MPI_Comm_free( &interComm );

        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], got connection from unknown "
                 "node %s. Ignoring it.\n", method_name, nodeId.nodeName);
        mon_log_write(MON_COMMACCEPT_7, SQ_LOG_ERR, buf);    

        return;
    }

    // Merge the inter-communicators obtained from the connect/accept
    // between this monitor and the connecting monitor.
    rc = MPI_Intercomm_merge( interComm, 0, &intraComm );
    if ( rc )
    {
        MPI_Comm_free( &interComm );

        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], Cannot merge intercomm: %s.\n",
                 method_name, ErrorMsg(rc));
        mon_log_write(MON_COMMACCEPT_5, SQ_LOG_ERR, buf);

        if ( MyNode->IsCreator() )
        {
            snprintf(buf, sizeof(buf), "Cannot merge intercomm for node %s: %s.\n",
                     nodeId.nodeName, ErrorMsg(rc));
            SQ_theLocalIOToClient->putOnNoticeQueue( MyNode->GetCreatorPid(),
                                                     Notice( buf ), NULL );
        }
        return;
    }

    MPI_Comm_set_errhandler( intraComm, MPI_ERRORS_RETURN );

    mem_log_write(CMonLog::MON_CONNTONEWMON_4, pnid);

    if ( MyNode->IsCreator() )
    {  // Send port and node info for existing nodes
        mem_log_write(CMonLog::MON_CONNTONEWMON_3, pnid);

        if ( !sendNodeInfo( interComm ) )
        {   // Had problem communicating with new monitor
            MPI_Comm_free( &intraComm );
            MPI_Comm_free( &interComm );

            char buf[MON_STRING_BUF_SIZE];
            snprintf(buf, sizeof(buf), "Cannot send node/port info to "
                     " node %s monitor: %s.\n", nodeId.nodeName, ErrorMsg(rc));
            SQ_theLocalIOToClient->putOnNoticeQueue( MyNode->GetCreatorPid(),
                                                     Notice( buf ), NULL );
            return;
        }

        Monitor->SetJoinComm( interComm );

        Monitor->SetIntegratingNid( pnid );

        Monitor->addNewComm( pnid, 1, intraComm );

        node->SetState( State_Merging ); 
    }
    else
    {   // No longer need inter-comm from "MPI_Comm_accept"

        Monitor->addNewComm( pnid, 1, intraComm );

        node->SetState( State_Merging ); 

        if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
        {
            trace_printf( "%s@%d - Sending ready flag to new monitor\n",
                          method_name, __LINE__);
        }

        // Tell connecting monitor that we are ready to integrate it.
        int readyFlag = 1;
        rc = Monitor->Send((char *) &readyFlag, sizeof(readyFlag), 0,
                           MON_XCHNG_DATA, interComm);
        if ( rc != MPI_SUCCESS )
        {
            MPI_Comm_free( &interComm );

            char buf[MON_STRING_BUF_SIZE];
            snprintf(buf, sizeof(buf), "[%s], unable to send connect "
                     "acknowledgement to new monitor: %s.\n", method_name,
                     ErrorMsg(rc));
            mon_log_write(MON_COMMACCEPT_9, SQ_LOG_ERR, buf);    

            if ( MyNode->IsCreator() )
            {
                snprintf(buf, sizeof(buf), "Cannot send connect acknowledgment "
                         "to new monitor: %s.\n", ErrorMsg(rc));
                SQ_theLocalIOToClient->putOnNoticeQueue( 
                            MyNode->GetCreatorPid(), Notice( buf ), NULL );
            }

            return;
        }

        MPI_Comm_free( &interComm );
    }

    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
    {
        trace_printf( "%s@%d - Connected to new monitor for node %d\n",
                      method_name, __LINE__, pnid );
    }

    // Ideally the following logic should be done in another thread
    // so this thread can post another accept without delay.  For
    // initial implementation simplicity this work is being done 
    // here for now
    if ( MyNode->IsCreator() )
    {
        mem_log_write(CMonLog::MON_CONNTONEWMON_5, pnid);

        // Get status from new monitor indicating whether
        // it is fully connected to other monitors.
        nodeStatus_t nodeStatus;
        rc = Monitor->Receive((char *) &nodeStatus,
                              sizeof(nodeStatus_t),
                              MPI_ANY_SOURCE, MON_XCHNG_DATA,
                              interComm);
        if ( rc != MPI_SUCCESS )
        {   // Handle error
            char buf[MON_STRING_BUF_SIZE];
            snprintf(buf, sizeof(buf), "[%s], unable to obtain "
                     "node status from new monitor: %s.\n",
                     method_name, ErrorMsg(rc));
            mon_log_write(MON_COMMACCEPT_8, SQ_LOG_ERR, buf);

            snprintf(buf, sizeof(buf), "Unable to obtain node status from "
                     "node %s monitor: %s.\n", nodeId.nodeName, ErrorMsg(rc));
            SQ_theLocalIOToClient->putOnNoticeQueue( MyNode->GetCreatorPid(),
                                                     Notice( buf ), NULL );

            node->SetState( State_Down );

            MPI_Comm_free ( &interComm );
            Monitor->SetJoinComm( MPI_COMM_NULL );
            Monitor->SetIntegratingNid( -1 );
        }
        else
        {
            mem_log_write(CMonLog::MON_CONNTONEWMON_6, node->GetPNid(),
                          nodeStatus.state);

            if (nodeStatus.state == State_Up)
            {
                // communicate the change and handle it after sync
                // in ImAlive
                node->SetChangeState( true );
            }
            else
            {
                char buf[MON_STRING_BUF_SIZE];
                snprintf(buf, sizeof(buf), "Node %s monitor failed to complete "
                         "initialization\n", nodeId.nodeName);
                SQ_theLocalIOToClient
                    ->putOnNoticeQueue( MyNode->GetCreatorPid(),
                                        Notice( buf ), NULL );

                node->SetState( State_Down ); 

                MPI_Comm_free ( &interComm );
                Monitor->SetJoinComm( MPI_COMM_NULL );
                Monitor->SetIntegratingNid( -1 );
            }
        }
    }

    TRACE_EXIT;
}

// commAcceptor thread main processing loop.  Keep an MPI_Comm_accept
// request outstanding.  After accepting a connection process it.
void CCommAccept::commAcceptor()
{
    const char method_name[] = "CCommAccept::commAcceptor";
    TRACE_ENTRY;

    int rc;
    int errClass;
    MPI_Comm interComm;

    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
    {
        trace_printf("%s@%d thread %lx starting\n", method_name,
                     __LINE__, thread_id_);
    }

    while (true)
    {
        if (trace_settings & TRACE_INIT)
        {
            trace_printf("%s@%d - Posting accept\n", method_name, __LINE__);
        }

        mem_log_write(CMonLog::MON_CONNTONEWMON_1);

        interComm = MPI_COMM_NULL;
        rc = MPI_Comm_accept( MyPort, MPI_INFO_NULL, 0, MPI_COMM_SELF,
                              &interComm );

        if (shutdown_)
        {   // We are being notified to exit.
            break;
        }

        if ( rc )
        {
            char buf[MON_STRING_BUF_SIZE];
            MPI_Error_class( rc, &errClass );
            snprintf(buf, sizeof(buf), "[%s], cannot accept new monitor: %s.\n",
                     method_name, ErrorMsg(rc));
            mon_log_write(MON_COMMACCEPT_2, SQ_LOG_ERR, buf);    

        }
        else
        {
sleep(30);
            processNewComm( interComm );
        }

    }

    if ( interComm != MPI_COMM_NULL ) MPI_Comm_free ( &interComm );

    if (trace_settings & TRACE_INIT)
        trace_printf("%s@%d thread %lx exiting\n", method_name,
                     __LINE__, pthread_self());

    TRACE_EXIT;

    pthread_exit(0);
}


void CCommAccept::shutdownWork(void)
{
    int rc;

    const char method_name[] = "CCommAccept::shutdownWork";
    TRACE_ENTRY;

    // Set flag that tells the commAcceptor thread to exit
    shutdown_ = true;   

    // Kill the commAccept thread
    if ((rc = pthread_kill(thread_id_, SIGKILL)) != 0)
    {
        char buf[MON_STRING_BUF_SIZE];
        sprintf(buf, "[%s], pthread_kill error=%d\n", method_name, rc);
        mon_log_write(MON_MLIO_SHUTDOWN_1, SQ_LOG_ERR, buf);
    }

    if (trace_settings & TRACE_INIT)
        trace_printf("%s@%d waiting for commAccept thread %lx to exit.\n",
                     method_name, __LINE__, thread_id_);

    // Wait for commAcceptor thread to exit
    pthread_join(thread_id_, NULL);

    TRACE_EXIT;
}

// Initialize commAcceptor thread
static void *commAccept(void *arg)
{
    const char method_name[] = "commAccept";
    TRACE_ENTRY;

    // Parameter passed to the thread is an instance of the CommAccept object
    CCommAccept *cao = (CCommAccept *) arg;

    // Mask all allowed signals 
    sigset_t  mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGPROF); // allows profiling such as google profiler
    int rc = pthread_sigmask(SIG_SETMASK, &mask, NULL);
    if (rc != 0)
    {
        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], pthread_sigmask error=%d\n",
                 method_name, rc);
        mon_log_write(MON_COMMACCEPT_1, SQ_LOG_ERR, buf);
    }

    // Enter thread processing loop
    cao->commAcceptor();

    TRACE_EXIT;
    return NULL;
}


// Create a commAcceptor thread
void CCommAccept::start()
{
    const char method_name[] = "CCommAccept::start";
    TRACE_ENTRY;

    int rc = pthread_create(&thread_id_, NULL, commAccept, this);
    if (rc != 0)
    {
        char buf[MON_STRING_BUF_SIZE];
        snprintf(buf, sizeof(buf), "[%s], thread create error=%d\n",
                 method_name, rc);
        mon_log_write(MON_COMMACCEPT_3, SQ_LOG_ERR, buf);
    }

    TRACE_EXIT;
}