//
//  AYPrintManager.cpp
//  DICOMPrint
//
//  Created by Alessandro Bettarini on 28 Dec 2018
//  Copyright © 2018 bettar. All rights reserved.
//
//  This file is licensed under GNU - GPL v3 license.
//  See file LICENCE for details.
//

#include <stdio.h>
#include "AYDcmPrintSCU.h"

#include "dcmtk/dcmpstat/dvpsdef.h"

static OFLogger printscuLogger = OFLog::getLogger("dcmtk.apps." OFFIS_CONSOLE_APPLICATION);

// See DCMTK dvpspr.cc line 31 DVPSPrintMessageHandler::DVPSPrintMessageHandler()
// See GingkoCAD dicomprintassociation.cpp line 174 PrintAssociation::PrintAssociation()
AYPrintManager::AYPrintManager()
: assoc(NULL)
, net(NULL)
, eventHandler(NULL)
, blockMode(DIMSE_BLOCKING)
, timeout(0)
{
}

// See DCMTK dvpspr.cc line 40 DVPSPrintMessageHandler::DVPSPrintMessageHandler()
AYPrintManager::~AYPrintManager()
{
    abortAssociation();
}

// See DCMTK dvpspr.cc line 45 DVPSPrintMessageHandler::dumpNMessage()
// See GingkoCAD dicomprintassociation.cpp line 661 PrintAssociation::dumpNMessage()
void AYPrintManager::dumpNMessage(T_DIMSE_Message &msg, DcmItem *dataset, OFBool outgoing)
{
    OFString str;
    if (outgoing) {
        DIMSE_dumpMessage(str, msg, DIMSE_OUTGOING, dataset);
    }
    else {
        DIMSE_dumpMessage(str, msg, DIMSE_INCOMING, dataset);
    }
    //OFLOG_DEBUG(printscuLogger, str);
    DCMPSTAT_DUMP(str);
}

// See DCMTK dvpspr.cc line 56 DVPSPrintMessageHandler::sendNRequest()
// See GingkoCAD dicomprintassociation.cpp line 672 PrintAssociation::sendNRequest()
OFCondition AYPrintManager::sendNRequest(
       T_ASC_PresentationContextID presId,
       T_DIMSE_Message &request,
       DcmDataset *rqDataSet,
       T_DIMSE_Message &response,
       DcmDataset* &statusDetail,
       DcmDataset* &rspDataset)
{
    OFCondition cond = EC_Normal;
    T_DIMSE_Command expectedResponse;
    DIC_US expectedMessageID=0;
    if (assoc == NULL)
        return DIMSE_ILLEGALASSOCIATION;
    
    T_DIMSE_DataSetType datasetType = DIMSE_DATASET_NULL;
    if (rqDataSet && (rqDataSet->card() > 0))
        datasetType = DIMSE_DATASET_PRESENT;
    
    switch(request.CommandField)
    {
        case DIMSE_N_GET_RQ:
            request.msg.NGetRQ.DataSetType = datasetType;
            expectedResponse = DIMSE_N_GET_RSP;
            expectedMessageID = request.msg.NGetRQ.MessageID;
            break;
        case DIMSE_N_SET_RQ:
            request.msg.NSetRQ.DataSetType = datasetType;
            expectedResponse = DIMSE_N_SET_RSP;
            expectedMessageID = request.msg.NSetRQ.MessageID;
            break;
        case DIMSE_N_ACTION_RQ:
            request.msg.NActionRQ.DataSetType = datasetType;
            expectedResponse = DIMSE_N_ACTION_RSP;
            expectedMessageID = request.msg.NActionRQ.MessageID;
            break;
        case DIMSE_N_CREATE_RQ:
            request.msg.NCreateRQ.DataSetType = datasetType;
            expectedResponse = DIMSE_N_CREATE_RSP;
            expectedMessageID = request.msg.NCreateRQ.MessageID;
            break;
        case DIMSE_N_DELETE_RQ:
            request.msg.NDeleteRQ.DataSetType = datasetType;
            expectedResponse = DIMSE_N_DELETE_RSP;
            expectedMessageID = request.msg.NDeleteRQ.MessageID;
            break;
        default:
            return DIMSE_BADCOMMANDTYPE;
            /* break; */
    }
    
    dumpNMessage(request, rqDataSet, OFTrue);
    cond = DIMSE_sendMessageUsingMemoryData(assoc, presId, &request, NULL, rqDataSet, NULL, NULL);
    if (cond.bad())
        return cond;
    
    T_ASC_PresentationContextID thisPresId;
    T_DIMSE_Message eventReportRsp;
    DIC_US eventReportStatus;
    do
    {
        thisPresId = presId;
        statusDetail = NULL;
        cond = DIMSE_receiveCommand(assoc, blockMode, this->timeout, &thisPresId, &response, &statusDetail);
        if (cond.bad())
            return cond;
        
        if (response.CommandField == DIMSE_N_EVENT_REPORT_RQ)
        {
            /* handle N-EVENT-REPORT-RQ */
            rspDataset = NULL;
            if (response.msg.NEventReportRQ.DataSetType == DIMSE_DATASET_PRESENT)
            {
                cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, this->timeout, &thisPresId, &rspDataset, NULL, NULL);
                if (cond.bad())
                    return cond;
            }
            dumpNMessage(response, rspDataset, OFFalse);
            // call event handler if registered
            eventReportStatus = STATUS_Success;
            if (eventHandler)
                eventReportStatus = eventHandler->handleEvent(response.msg.NEventReportRQ, rspDataset, statusDetail);

            if (rspDataset) {
                delete rspDataset;
                rspDataset = NULL;
            }

            if (statusDetail) {
                delete statusDetail;
                statusDetail = NULL;
            }
            
            // send back N-EVENT-REPORT-RSP */
            eventReportRsp.CommandField = DIMSE_N_EVENT_REPORT_RSP;
            eventReportRsp.msg.NEventReportRSP.MessageIDBeingRespondedTo = response.msg.NEventReportRQ.MessageID;
            eventReportRsp.msg.NEventReportRSP.EventTypeID = response.msg.NEventReportRQ.EventTypeID;
            eventReportRsp.msg.NEventReportRSP.DimseStatus = eventReportStatus;
            eventReportRsp.msg.NEventReportRSP.DataSetType = DIMSE_DATASET_NULL;
            eventReportRsp.msg.NEventReportRSP.opts = O_NEVENTREPORT_EVENTTYPEID;
            eventReportRsp.msg.NEventReportRSP.AffectedSOPClassUID[0] = 0;
            eventReportRsp.msg.NEventReportRSP.AffectedSOPInstanceUID[0] = 0;
            dumpNMessage(eventReportRsp, NULL, OFTrue);
            cond = DIMSE_sendMessageUsingMemoryData(assoc, thisPresId, &eventReportRsp, NULL, NULL, NULL, NULL);
            if (cond.bad())
                return cond;
        }
        else {
            /* No N-EVENT-REPORT-RQ. Check if this message is what we expected */
            if (response.CommandField != expectedResponse)
            {
                char buf1[256];
                sprintf(buf1, "DIMSE: Unexpected Response Command Field: 0x%x", (unsigned)response.CommandField);
                return makeDcmnetCondition(DIMSEC_UNEXPECTEDRESPONSE, OF_error, buf1);
            }

            T_DIMSE_DataSetType responseDataset = DIMSE_DATASET_NULL;
            DIC_US responseMessageID = 0;
            /** change request to response */
            switch(expectedResponse)
            {
                case DIMSE_N_GET_RSP:
                    responseDataset = response.msg.NGetRSP.DataSetType;
                    responseMessageID = response.msg.NGetRSP.MessageIDBeingRespondedTo;
                    break;
                case DIMSE_N_SET_RSP:
                    responseDataset = response.msg.NSetRSP.DataSetType;
                    responseMessageID = response.msg.NSetRSP.MessageIDBeingRespondedTo;
                    break;
                case DIMSE_N_ACTION_RSP:
                    responseDataset = response.msg.NActionRSP.DataSetType;
                    responseMessageID = response.msg.NActionRSP.MessageIDBeingRespondedTo;
                    break;
                case DIMSE_N_CREATE_RSP:
                    responseDataset = response.msg.NCreateRSP.DataSetType;
                    responseMessageID = response.msg.NCreateRSP.MessageIDBeingRespondedTo;
                    break;
                case DIMSE_N_DELETE_RSP:
                    responseDataset = response.msg.NDeleteRSP.DataSetType;
                    responseMessageID = response.msg.NDeleteRSP.MessageIDBeingRespondedTo;
                    break;
                default:
                {
                    char buf1[256];
                    sprintf(buf1, "DIMSE: Unexpected Response Command Field: 0x%x", (unsigned)response.CommandField);
                    return makeDcmnetCondition(DIMSEC_UNEXPECTEDRESPONSE, OF_error, buf1);
                }
                    /* break; */
            }

            if (responseMessageID != expectedMessageID)
            {
                char buf1[256];
                sprintf(buf1, "DIMSE: Unexpected Response Command Field: 0x%x", (unsigned)response.CommandField);
                return makeDcmnetCondition(DIMSEC_UNEXPECTEDRESPONSE, OF_error, buf1);
            }

            rspDataset = NULL;
            if (responseDataset == DIMSE_DATASET_PRESENT)
            {
                cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, this->timeout, &thisPresId, &rspDataset, NULL, NULL);
                if (cond.bad())
                    return cond;
            }

            dumpNMessage(response, rspDataset, OFFalse);
        }
    }
    while (response.CommandField == DIMSE_N_EVENT_REPORT_RQ);

    return EC_Normal;
}

// See DCMTK dvpspr.cc line 210 DVPSPrintMessageHandler::createRQ()
// See GingkoCAD dicomprintassociation.cpp line 826 PrintAssociation::createRQ()
OFCondition AYPrintManager::createRQ(
   const char *sopclassUID,
   OFString& sopinstanceUID,
   DcmDataset *attributeListIn,
   Uint16& status,
   DcmDataset* &attributeListOut)
{
    if (assoc == NULL)
        return DIMSE_ILLEGALASSOCIATION;

    if (sopclassUID==NULL)
        return DIMSE_NULLKEY;
    
    T_ASC_PresentationContextID presCtx = findAcceptedPC(sopclassUID);
    if (presCtx == 0)
        return DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    
    T_DIMSE_Message request;
    T_DIMSE_Message response;
    DcmDataset *statusDetail = NULL;
    
    // construct N-CREATE-RQ
    request.CommandField = DIMSE_N_CREATE_RQ;
    request.msg.NCreateRQ.MessageID = assoc->nextMsgID++;
    strcpy(request.msg.NCreateRQ.AffectedSOPClassUID, sopclassUID);
    if (sopinstanceUID.size() > 0)
    {
        strcpy(request.msg.NCreateRQ.AffectedSOPInstanceUID, sopinstanceUID.c_str());
        request.msg.NCreateRQ.opts = O_NCREATE_AFFECTEDSOPINSTANCEUID;
    }
    else {
        request.msg.NCreateRQ.AffectedSOPInstanceUID[0] = 0;
        request.msg.NCreateRQ.opts = 0;
    }
    
    OFCondition cond = sendNRequest(presCtx, request, attributeListIn, response, statusDetail, attributeListOut);
    if (statusDetail)
        delete statusDetail;

    if (cond.good())
    {
        status = response.msg.NCreateRSP.DimseStatus;
        if (status != STATUS_Success)
            OFLOG_ERROR(printscuLogger, "Error sending Create RQ, status: " << status);

        // if response contains SOP Instance UID, copy it.
        if (response.msg.NCreateRSP.opts & O_NCREATE_AFFECTEDSOPINSTANCEUID)
        {
            sopinstanceUID = response.msg.NCreateRSP.AffectedSOPInstanceUID;
        }
    }

    return cond;
}

// See DCMTK dvpspr.cc line 263 DVPSPrintMessageHandler::setRQ()
// See GingkoCAD dicomprintassociation.cpp line 826 PrintAssociation::setRQ()
OFCondition AYPrintManager::setRQ(
    const char *sopclassUID,
    const char *sopinstanceUID,
    DcmDataset *modificationList,
    Uint16& status,
    DcmDataset* &attributeListOut)
{
    if (assoc == NULL)
        return DIMSE_ILLEGALASSOCIATION;

    if ((sopclassUID==NULL)||(sopinstanceUID==NULL)||(modificationList==NULL))
        return DIMSE_NULLKEY;
    
    T_ASC_PresentationContextID presCtx = findAcceptedPC(sopclassUID);
    if (presCtx == 0)
        return DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    
    T_DIMSE_Message request;
    T_DIMSE_Message response;
    DcmDataset *statusDetail = NULL;
    
    // construct N-SET-RQ
    request.CommandField = DIMSE_N_SET_RQ;
    request.msg.NSetRQ.MessageID = assoc->nextMsgID++;
    strcpy(request.msg.NSetRQ.RequestedSOPClassUID, sopclassUID);
    strcpy(request.msg.NSetRQ.RequestedSOPInstanceUID, sopinstanceUID);
    
    OFCondition cond = sendNRequest(presCtx, request, modificationList, response, statusDetail, attributeListOut);
    if (cond.good())
        status = response.msg.NSetRSP.DimseStatus;

    if (status != STATUS_Success)
        OFLOG_ERROR(printscuLogger, "Error sending Set RQ, status: " << status);

    if (statusDetail)
        delete statusDetail;

    return cond;
}

// See DCMTK dvpspr.cc line 301 DVPSPrintMessageHandler::getRQ()
// See GingkoCAD dicomprintassociation.cpp line 931 PrintAssociation::getRQ()
OFCondition AYPrintManager::getRQ(
    const char *sopclassUID,
    const char *sopinstanceUID,
    const Uint16 *attributeIdentifierList,
    size_t numShorts,
    Uint16& status,
    DcmDataset* &attributeListOut)
{
    if (assoc == NULL)
    {
        OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);
        return DIMSE_ILLEGALASSOCIATION;
    }

    if ((sopclassUID==NULL)||(sopinstanceUID==NULL))
    {
        OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);
        return DIMSE_NULLKEY;
    }
    
    T_ASC_PresentationContextID presCtx = findAcceptedPC(sopclassUID);
    if (presCtx == 0)
    {
        OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);
        return DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    }
    
    T_DIMSE_Message request;
    T_DIMSE_Message response;
    DcmDataset *statusDetail = NULL;
    
    // construct N-GET-RQ
    request.CommandField = DIMSE_N_GET_RQ;
    request.msg.NGetRQ.MessageID = assoc->nextMsgID++;
    strcpy(request.msg.NGetRQ.RequestedSOPClassUID, sopclassUID);
    strcpy(request.msg.NGetRQ.RequestedSOPInstanceUID, sopinstanceUID);
    request.msg.NGetRQ.ListCount = 0;
    if (attributeIdentifierList)
        request.msg.NGetRQ.ListCount = (int)numShorts;

    request.msg.NGetRQ.AttributeIdentifierList = (DIC_US *)attributeIdentifierList;
    
    OFCondition cond = sendNRequest(presCtx, request, NULL, response, statusDetail, attributeListOut);
    if (cond.good())
        status = response.msg.NGetRSP.DimseStatus;

    if (status != STATUS_Success)
        OFLOG_ERROR(printscuLogger, "Error sending Get RQ, status: " << status);

    if (statusDetail)
        delete statusDetail;

    return cond;
}

// See DCMTK dvpspr.cc line 344 DVPSPrintMessageHandler::actionRQ()
// See GingkoCAD dicomprintassociation.cpp line 983 PrintAssociation::actionRQ()
OFCondition AYPrintManager::actionRQ(
   const char *sopclassUID,
   const char *sopinstanceUID,
   Uint16 actionTypeID,
   DcmDataset *actionInformation,
   Uint16& status,
   DcmDataset* &actionReply)
{
    if (assoc == NULL)
        return DIMSE_ILLEGALASSOCIATION;

    if ((sopclassUID==NULL) || (sopinstanceUID==NULL))
        return DIMSE_NULLKEY;
    
    T_ASC_PresentationContextID presCtx = findAcceptedPC(sopclassUID);
    if (presCtx == 0)
        return DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    
    T_DIMSE_Message request;
    T_DIMSE_Message response;
    DcmDataset *statusDetail = NULL;
    
    // construct N-ACTION-RQ
    request.CommandField = DIMSE_N_ACTION_RQ;
    request.msg.NActionRQ.MessageID = assoc->nextMsgID++;
    strcpy(request.msg.NActionRQ.RequestedSOPClassUID, sopclassUID);
    strcpy(request.msg.NActionRQ.RequestedSOPInstanceUID, sopinstanceUID);
    request.msg.NActionRQ.ActionTypeID = (DIC_US)actionTypeID;
    
    OFCondition cond = sendNRequest(presCtx, request, actionInformation, response, statusDetail, actionReply);
    if (cond.good())
        status = response.msg.NActionRSP.DimseStatus;

    if (status != STATUS_Success)
        OFLOG_ERROR(printscuLogger, "Error sending Action RQ, status: " << status);

    if (statusDetail)
        delete statusDetail;

    return cond;
}

// See DCMTK dvpspr.cc line 384 DVPSPrintMessageHandler::deleteRQ()
// See GingkoCAD dicomprintassociation.cpp line 1030 PrintAssociation::deleteRQ()
OFCondition AYPrintManager::deleteRQ(
                                       const char *sopclassUID,
                                       const char *sopinstanceUID,
                                       Uint16& status)
{
    if (assoc == NULL)
        return DIMSE_ILLEGALASSOCIATION;

    if ((sopclassUID==NULL) || (sopinstanceUID==NULL))
        return DIMSE_NULLKEY;
    
    T_ASC_PresentationContextID presCtx = findAcceptedPC(sopclassUID);
    if (presCtx == 0)
        return DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    
    T_DIMSE_Message request;
    T_DIMSE_Message response;
    DcmDataset *statusDetail = NULL;
    DcmDataset *attributeListOut = NULL;
    
    // construct N-DELETE-RQ
    request.CommandField = DIMSE_N_DELETE_RQ;
    request.msg.NDeleteRQ.MessageID = assoc->nextMsgID++;
    strcpy(request.msg.NDeleteRQ.RequestedSOPClassUID, sopclassUID);
    strcpy(request.msg.NDeleteRQ.RequestedSOPInstanceUID, sopinstanceUID);
    
    OFCondition cond = sendNRequest(presCtx, request, NULL, response, statusDetail, attributeListOut);
    if (cond.good())
        status = response.msg.NDeleteRSP.DimseStatus;

    if (status != STATUS_Success)
        OFLOG_ERROR(printscuLogger, "Error sending Delete RQ, status: " << status);

    if (statusDetail)
        delete statusDetail;

    if (attributeListOut)
        delete attributeListOut;  // should never happen

    return cond;
}

// See DCMTK dvpspr.cc line 422 DVPSPrintMessageHandler::releaseAssociation()
// See GingkoCAD dicomprintassociation.cpp line 1075 PrintAssociation::releaseAssociation()
OFCondition AYPrintManager::releaseAssociation()
{
    OFLOG_TRACE(printscuLogger, __FUNCTION__ << __LINE__);
    OFCondition result = EC_Normal;
    if (assoc) {
        result = ASC_releaseAssociation(assoc);
        ASC_destroyAssociation(&assoc);
        ASC_dropNetwork(&net);
        assoc = NULL;
        net = NULL;
    }

    return result;
}

// See DCMTK dvpspr.cc line 436 DVPSPrintMessageHandler::abortAssociation()
// See GingkoCAD dicomprintassociation.cpp line 1088 PrintAssociation::abortAssociation()
OFCondition AYPrintManager::abortAssociation()
{
    OFCondition result = EC_Normal;
    if (assoc) {
        result = ASC_abortAssociation(assoc);
        ASC_destroyAssociation(&assoc);
        ASC_dropNetwork(&net);
        net = NULL;
        assoc = NULL;
    }

    return result;
}

// See DCMTK dvpspr.cc line 450 DVPSPrintMessageHandler::findAcceptedPC()
// See GingkoCAD dicomprintassociation.cpp line 1100 PrintAssociation::findAcceptedPC()
T_ASC_PresentationContextID AYPrintManager::findAcceptedPC(const char *sopclassuid)
{
    if ((assoc==NULL) || (sopclassuid==NULL))
        return 0;
    
    // if the SOP class is one of the Basic Grayscale Print Management Meta SOP Classes,
    // look for a presentation context for Basic Grayscale Print.
    OFString sopclass(sopclassuid);
    if ((sopclass == UID_BasicFilmSessionSOPClass) ||
        (sopclass == UID_BasicFilmBoxSOPClass) ||
        (sopclass == UID_BasicGrayscaleImageBoxSOPClass) ||
        (sopclass == UID_PrinterSOPClass))
    {
        sopclassuid = UID_BasicGrayscalePrintManagementMetaSOPClass;
    }

    return ASC_findAcceptedPresentationContextID(assoc, sopclassuid);
}

// See DCMTK dvpspr.cc line 465 DVPSPrintMessageHandler::negotiateAssociation()
// See GingkoCAD dicomprintassociation.cpp line 1177 PrintAssociation::negotiateAssociation()
OFCondition AYPrintManager::negotiateAssociation(
     DcmTransportLayer *tlayer,
     const char *myAEtitle,
     const char *peerAEtitle,
     const char *peerHost,
     int peerPort,
     long peerMaxPDU,
     OFBool negotiatePresentationLUT,
     OFBool negotiateAnnotationBox,
     OFBool negotiateColorjob,
     OFBool implicitOnly)
{
    //OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);

    if (assoc) {
        OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);
        return makeDcmnetCondition(DIMSEC_ILLEGALASSOCIATION, OF_error, "association already in place");
    }
    
    if ((myAEtitle==NULL) || (peerAEtitle==NULL) || (peerHost==NULL))
    {
        OFLOG_DEBUG(printscuLogger, __FUNCTION__ << __LINE__);
        return DIMSE_NULLKEY;
    }
    
    OFCondition cond;

    T_ASC_Parameters *params=NULL;
    DIC_NODENAME dnpeerHost;

    // TODO: check validity of net

    // See DCMTK dvpspr.cc line 488
    cond = ASC_initializeNetwork(NET_REQUESTOR, 0, 30, &net);
    if (cond.good())
        cond = ASC_createAssociationParameters(&params, peerMaxPDU);

    if (tlayer && cond.good())
    {
        cond = ASC_setTransportLayer(net, tlayer, 0);
        if (cond.good())
            cond = ASC_setTransportLayerType(params, OFTrue /* use TLS */);
    }
    
    if (cond.bad()) {
        OFLOG_ERROR(printscuLogger, __FUNCTION__ << __LINE__ << cond.text());
        return cond;
    }
    
    ASC_setAPTitles(params, myAEtitle, peerAEtitle, NULL);
    sprintf(dnpeerHost, "%s:%d", peerHost, peerPort);
    ASC_setPresentationAddresses(params, OFStandard::getHostName().c_str(), dnpeerHost);
    
    /* presentation contexts */
    const char* transferSyntaxes[3];
    int transferSyntaxCount = 0;
    
    if (implicitOnly)
    {
        OFLOG_TRACE(printscuLogger, __FUNCTION__ << __LINE__);
        transferSyntaxes[0] = UID_LittleEndianImplicitTransferSyntax;
        transferSyntaxCount = 1;
    }
    else {
        OFLOG_TRACE(printscuLogger, __FUNCTION__ << __LINE__);
        /* gLocalByteOrder is defined in dcxfer.h */
        if (gLocalByteOrder == EBO_LittleEndian) {
            /* we are on a little endian machine */
            transferSyntaxes[0] = UID_LittleEndianExplicitTransferSyntax;
            transferSyntaxes[1] = UID_BigEndianExplicitTransferSyntax;
        }
        else {
            /* we are on a big endian machine */
            transferSyntaxes[0] = UID_BigEndianExplicitTransferSyntax;
            transferSyntaxes[1] = UID_LittleEndianExplicitTransferSyntax;
        }
        transferSyntaxes[2] = UID_LittleEndianImplicitTransferSyntax;
        transferSyntaxCount = 3;
    }

    /* we always propose basic grayscale, presentation LUT and annotation box*/
    if (cond.good())
        cond = ASC_addPresentationContext(params, 1, UID_BasicGrayscalePrintManagementMetaSOPClass, transferSyntaxes, transferSyntaxCount);

    if (negotiatePresentationLUT)
    {
        if (cond.good())
            cond = ASC_addPresentationContext(params, 3, UID_PresentationLUTSOPClass, transferSyntaxes, transferSyntaxCount);
    }
    
    if (negotiateAnnotationBox)
    {
        if (cond.good())
            cond = ASC_addPresentationContext(params, 5, UID_BasicAnnotationBoxSOPClass, transferSyntaxes, transferSyntaxCount);
    }
    
    /* create association */
    OFLOG_INFO(printscuLogger, "Requesting Association");

    if (cond.good())
    {
        cond = ASC_requestAssociation(net, params, &assoc);
        
        if (cond == DUL_ASSOCIATIONREJECTED)
        {
            OFString temp_str;
            T_ASC_RejectParameters rej;
            ASC_getRejectParameters(params, &rej);
            OFLOG_WARN(printscuLogger, "Association Rejected" << OFendl << ASC_printRejectParameters(temp_str, &rej));
        }
        else {
            if (cond.bad())
            {
                OFLOG_ERROR(printscuLogger, __FUNCTION__ << __LINE__ << cond.text());

                // if assoc is non-NULL, then params has already been moved into the
                // assoc structure. Make sure we only delete once!
                if (assoc)
                    ASC_destroyAssociation(&assoc);
                else if (params)
                    ASC_destroyAssociationParameters(&params);
                
                if (net)
                    ASC_dropNetwork(&net);

                assoc = NULL;
                net = NULL;
                return cond;
            }
        }
    }
    
    if ((cond.good()) && (0 == ASC_findAcceptedPresentationContextID(assoc, UID_BasicGrayscalePrintManagementMetaSOPClass)))
    {
        OFLOG_WARN(printscuLogger, __FUNCTION__ << __LINE__ << "Peer does not support Basic Grayscale Print Management, aborting association.");
        abortAssociation();
        cond = DIMSE_NOVALIDPRESENTATIONCONTEXTID;
    }

    if (cond.good())
    {
        DCMPSTAT_INFO("Association accepted (Max Send PDV: " << assoc->sendPDVLength << ")");
    }
    else {
        OFLOG_WARN(printscuLogger, __FUNCTION__ << __LINE__);
        // params is now an alias to assoc->params. Don't call ASC_destroyAssociationParameters.
        if (assoc)
            ASC_destroyAssociation(&assoc);
        
        if (net)
            ASC_dropNetwork(&net);

        assoc = NULL;
        net = NULL;
    }
    
    return cond;
}
