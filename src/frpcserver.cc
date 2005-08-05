/*
 * FILE          $Id: frpcserver.cc,v 1.3 2005-08-05 08:14:30 vasek Exp $
 *
 * DESCRIPTION   
 *
 * AUTHOR        
 *              Miroslav Talasek <miroslav.talasek@firma.seznam.cz>
 *
 * Copyright (C) Seznam.cz a.s. 2002
 * All Rights Reserved
 *
 * HISTORY
 *       
 */
#include "frpcserver.h"

#include <string>
#include <algorithm>
#include <sstream>
#include <strstream>
#include <memory>
#include <stdexcept>
#include <frpchttpclient.h>
#include <frpcstreamerror.h>
#include <frpchttpclient.h>
#include <frpctreebuilder.h>
#include <frpcunmarshaller.h>
#include <frpcmarshaller.h>
#include <frpchttperror.h>
#include <frpcinternals.h>
#include <frpc.h>
#include <frpcsocket.h>


namespace FRPC
{


Server_t::~Server_t()
{}

void Server_t::serve(int fd, struct sockaddr_in* addr )
{
    // prepare query storage
    queryStorage.push_back(std::string());
    queryStorage.back().reserve(BUFFER_SIZE);
    contentLength = 0;
    closeConnection = false;
    headersSent = false;
    head = false;

    std::string clientIP;

    io.setSocket(fd);

    if(!addr)
    {
        struct sockaddr_in clientaddr;
        socklen_t sinSize = sizeof( struct sockaddr_in );
        if(getpeername(fd,(struct sockaddr*) &clientaddr, &sinSize) == 0 )
        {
            clientIP = inet_ntoa(clientaddr.sin_addr);
        }
        else
        {
            clientIP = "unknown";
        }
    }
    else
    {
        clientIP = inet_ntoa(addr->sin_addr);
    }

    long requestCount = 0;


    do
    {
        Pool_t pool;
        TreeBuilder_t builder(pool);



        try
        {
            readRequest(builder);

        }
        catch(const StreamError_t &streamError)
        {
            std::auto_ptr<Marshaller_t> marshaller(Marshaller_t::create(
                                                       ((outType ==
                                                         BINARY_RPC)?Marshaller_t::BINARY_RPC
                                                        :Marshaller_t::XML_RPC),*this));

            marshaller->packFault(MethodRegistry_t::FRPC_PARSE_ERROR, streamError.message().c_str());
            marshaller->flush();


            continue;

        }
        catch(const HTTPError_t &httpError)
        {
            sendHttpError(httpError);

            break;
        }


        //printf("method name %s \n",builder.getUnMarshaledMethodName().c_str());

        try
        {

            if (head)
            {
                long result = methodRegistry.headCall();
                if (result == 0)
                    flush();
                else if (result < 0)
                    throw HTTPError_t(HTTP_METHOD_NOT_ALLOWED,"Method Not Allowed");
                else
                    throw HTTPError_t(HTTP_SERVICE_UNAVAILABLE,"Service Unavailable");
            }
            else
            {
                methodRegistry.processCall(clientIP, builder.getUnMarshaledMethodName(),
                                           Array(builder.getUnMarshaledData()),*this,
                                           ((outType ==
                                             BINARY_RPC)?Marshaller_t::BINARY_RPC
                                            :Marshaller_t::XML_RPC));
            }
        }

        catch(const HTTPError_t &httpError)
        {
            sendHttpError(httpError);

            break;
        }

        catch(...)
        {
            break;
        }

        requestCount++;


    }
    while(!(closeConnection == true || keepAlive == false || requestCount >= maxKeepalive));
}

void Server_t::readRequest(DataBuilder_t &builder)
{
    HTTPHeader_t httpHead;

    std::string protocol;
    std::string transferMethod;
    std::string contentType;
    std::auto_ptr<UnMarshaller_t> unmarshaller;
    //SocketCloser_t closer(httpIO.socket());

    //read hlavicku
    try
    {
        // ln je èíslo øádky
        // read all lines until first non-empty
        for (;;)
        {
            // read line from the socket, we check security limits for url
            std::string line(io.readLine(true));

            if (line.empty())
                continue;
            // break request line down
            std::vector<std::string> header(io.splitBySpace(line, 3));
            if (header.size() != 3)
            {
                // invalid request line
                // get rid of old method
                //lastMethod.erase();
                throw HTTPError_t(HTTP_BAD_REQUEST, "Bad HTTP request: '%s'.",
                                  line.substr(0, 30).c_str());
            }
            protocol =  header[2];
            // save request line parts
            if(protocol != "HTTP/1.1" && protocol != "HTTP/1.0")
            {
                throw HTTPError_t(HTTP_HTTP_VERSION_NOT_SUPPORTED,
                                  "Bad HTTP protocol version or type: '%s'.",
                                  header[2].c_str());
            }

            transferMethod =  header[0];


            /*std::istringstream is(header[1].c_str());
            int status;
            is >> status;
            if(status != 200)
            {
                throw HTTPError_t(status, "%s",header[2].c_str());
            }*/

            break;
        }

        // read header from the request
        io.readHeader(httpHead);

        if(transferMethod != "POST")
        {

            if(transferMethod == "HEAD")
            {
                head = true;
                closeConnection = true;
                return;
            }
            else
            {
                throw HTTPError_t(HTTP_METHOD_NOT_ALLOWED, "Method Not Allowed");
            }
        }


        // get content type from header
        httpHead.get(HTTP_HEADER_CONTENT_TYPE, contentType );
        //create unmarshaller and datasink

        //ake typy podporuje server ?
        outType = XML_RPC;
        useChunks = false;

        std::string accept("");
        if( httpHead.get(HTTP_HEADER_ACCEPT, accept) == 0)
        {


            if(accept.find("text/xml") != std::string::npos)
            {
                outType = XML_RPC;
                useChunks = false;
            }

            if(accept.find("application/x-frpc") != std::string::npos)
            {
                outType = BINARY_RPC;
                useChunks = true;
            }
        }



        if(contentType.find("text/xml") != std::string::npos )
        {
            std::auto_ptr<UnMarshaller_t> ptr(
                UnMarshaller_t::create(UnMarshaller_t::XML_RPC, builder));
            unmarshaller = ptr;
        }
        else if (contentType.find("application/x-frpc")!= std::string::npos )
        {
            std::auto_ptr<UnMarshaller_t> ptr(
                UnMarshaller_t::create(UnMarshaller_t::BINARY_RPC, builder));
            unmarshaller = ptr;
        }

        else
        {
            throw StreamError_t("Unknown ContentType");
        }



        DataSink_t data(*unmarshaller, UnMarshaller_t::TYPE_METHOD_CALL);



        // read body of response
        io.readContent(httpHead, data, false);

        unmarshaller->finish();


        std::string connection;
        httpHead.get("Connection", connection);
        std::transform(connection.begin(), connection.end(),
                       connection.begin(), upper());
        closeConnection = false;

        if (protocol == "HTTP/1.1")
        {
            closeConnection = (connection == "CLOSE");
        }
        else
        {
            closeConnection = (connection != "KEEP-ALIVE");
        }




    }
    catch (const Error_t &)
    {
        // get rid of old method

        throw;
    }

    /*// close socket
    if (!((!keepAlive || closeConnection) && (httpIO.socket() > -1)))
    {
        closer.doClose = false;
    } */



}
void Server_t::write(const char* data, long size)
{

    contentLength += size;

    if(size > long(BUFFER_SIZE - queryStorage.back().size()))
    {
        if(useChunks)
        {
            sendResponse();
            queryStorage.back().append(data, size);
        }
        else
        {
            if(size > BUFFER_SIZE)
            {
                queryStorage.push_back(std::string(data, size));
            }
            else
            {
                queryStorage.back().append(data, size);
            }
        }
    }
    else
    {
        queryStorage.back().append(data, size);
    }

}

void Server_t::flush()
{
    if(!useChunks)
    {
        sendResponse();

    }
    else
    {
        sendResponse();
        io.sendData("0\r\n\r\n", 5);
    }


}

void Server_t::sendHttpError(const HTTPError_t &httpError)
{
    StreamHolder_t os;
    //create header
    os.os << "HTTP/1.1" << ' ' << httpError.errorNum() << ' ' << httpError.message() << "\r\n";
    os.os  << HTTP_HEADER_ACCEPT << ": " << "text/xml, application/x-frpc"<< "\r\n";
    os.os << "Server:" << " Fast-RPC  Server Linux\r\n";

    // terminate header
    // append separator
    os.os << "\r\n";
    // send header
    io.sendData(os.os.str(), os.os.pcount());
}

void Server_t::sendResponse()
{

    if(!headersSent)
    {
        HTTPHeader_t header;
        StreamHolder_t os;


        //create header
        os.os << "HTTP/1.1" << ' ' << "200" << ' ' << "OK" << "\r\n";

        //os.os << "Host: " << url.host << ':' << url.port << "\r\n";
        switch(outType)
        {
        case XML_RPC:
            {

                os.os << HTTP_HEADER_CONTENT_TYPE << ": " << "text/xml"<< "\r\n";
            }
            break;
        case BINARY_RPC:
            {
                os.os << HTTP_HEADER_CONTENT_TYPE << ": " << "application/x-frpc"<< "\r\n";
            }
            break;

        default:
            throw StreamError_t("Unknown protocol");
            break;

        }

        os.os  << HTTP_HEADER_ACCEPT << ": " << "text/xml, application/x-frpc"<< "\r\n";

        //append connection header
        os.os << HTTP_HEADER_CONNECTION << (keepAlive ? ": keep-alive" : ": close")
        << "\r\n";

        // write content-length or content-transfer-encoding when we can send
        // content

        if (!useChunks)
        {
            os.os << HTTP_HEADER_CONTENT_LENGTH << ": " << contentLength
            << "\r\n";
        }
        else
        {
            os.os << HTTP_HEADER_TRANSFER_ENCODING << ": chunked\r\n";
        }

        os.os << "Server:" << " Fast-RPC  Server Linux\r\n";

        // terminate header
        // append separator
        os.os << "\r\n";


        // send header
        io.sendData(os.os.str(), os.os.pcount());

        headersSent = true;

        if(head)
            return;

    }

    if(useChunks)
    {
        // write chunk size
        StreamHolder_t os;
        os.os << std::hex << queryStorage.back().size() << "\r\n";
        io.sendData(os.os.str(), os.os.pcount());
        // write chunk
        io.sendData(queryStorage.back().data(),queryStorage.back().size() );
        // write chunk terminator
        io.sendData("\r\n", 2);
        queryStorage.back().erase();


    }

    else
    {
        while(queryStorage.size() != 1)
        {
            io.sendData(queryStorage.back().data(),queryStorage.back().size() );
            queryStorage.pop_back();
        }
        io.sendData(queryStorage.back().data(),queryStorage.back().size() );
        queryStorage.back().erase();


    }

}

}
