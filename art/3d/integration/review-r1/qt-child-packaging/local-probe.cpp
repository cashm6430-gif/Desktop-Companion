#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <iostream>
#include <windows.h>
QJsonObject tokenFacts(HANDLE process){
 QJsonObject o;HANDLE token=nullptr;
 if(!OpenProcessToken(process,TOKEN_QUERY,&token)){o["error"]=int(GetLastError());return o;}
 o["restricted"]=bool(IsTokenRestricted(token));DWORD size=0;TOKEN_ELEVATION e={};
 if(GetTokenInformation(token,TokenElevation,&e,sizeof(e),&size))o["elevated"]=bool(e.TokenIsElevated);
 GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&size);
 QByteArray b(size,0);
 if(GetTokenInformation(token,TokenIntegrityLevel,b.data(),size,&size)){auto* m=(TOKEN_MANDATORY_LABEL*)b.data(); o["integrity_rid"]=int(*GetSidSubAuthority(m->Label.Sid,*GetSidSubAuthorityCount(m->Label.Sid)-1));}
 CloseHandle(token);return o;
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QString mode=app.arguments().value(1);QString endpoint=app.arguments().value(2);QJsonObject facts;facts["token"]=tokenFacts(GetCurrentProcess());facts["mode"]=mode;facts["endpoint"]=endpoint;
 if(mode=="--client"){QLocalSocket socket;socket.connectToServer(endpoint);bool connected=socket.waitForConnected(1500);facts["connected"]=connected;bool written=false,reply=false;if(connected){socket.write("probe\n");written=socket.waitForBytesWritten(1000);reply=socket.waitForReadyRead(1000)&&socket.readAll().trimmed()=="ok";}facts["written"]=written;facts["reply"]=reply;facts["error"]=socket.errorString();std::cout<<QJsonDocument(facts).toJson().toStdString();return reply?0:1;}
 QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);facts["listen"]=server.listen(endpoint);std::cout<<QJsonDocument(facts).toJson(QJsonDocument::Compact).toStdString()<<std::endl;
 QObject::connect(&server,&QLocalServer::newConnection,&app,[&]{while(server.hasPendingConnections()){auto*socket=server.nextPendingConnection();QObject::connect(socket,&QLocalSocket::readyRead,&app,[&,socket]{if(socket->readAll().contains("probe")){socket->write("ok\n");socket->flush();}});QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);}});QTimer::singleShot(8000,&app,&QCoreApplication::quit);return app.exec();}
