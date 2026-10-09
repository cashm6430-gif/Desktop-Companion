#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
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
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject all;all["parent_token"]=tokenFacts(GetCurrentProcess());QJsonObject env;auto environment=QProcessEnvironment::systemEnvironment();for(auto k:{"APPDATA","LOCALAPPDATA","USERPROFILE","TEMP","TMP","USERNAME","USERDOMAIN","SYSTEMROOT","WINDIR"})env[k]=environment.value(k);all["environment"]=env;QJsonArray runs;
 for(auto mode:{"default","hide","hide_no_window"})for(auto cwd:{"E:/projects/Desktop-Companion","E:/projects/Desktop-Companion/.local/authoring/toon-host-20261009-gpu-r1/project"}){
  QProcess p;p.setProgram("D:/tool/godot/Godot_v4.7.2-stable_win64.exe");p.setArguments({"--headless","--path","E:/projects/Desktop-Companion/.local/authoring/toon-host-20261009-gpu-r1/project","--quit-after","2"});p.setWorkingDirectory(cwd);p.setProcessChannelMode(QProcess::MergedChannels);
  if(QString(mode)!="default")p.setCreateProcessArgumentsModifier([mode](QProcess::CreateProcessArguments*a){a->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;a->startupInfo->wShowWindow=SW_HIDE;if(QString(mode)=="hide_no_window")a->flags|=CREATE_NO_WINDOW;});
  p.start();QJsonObject run;run["mode"]=mode;run["cwd"]=cwd;run["started"]=p.waitForStarted(5000);HANDLE child=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(p.processId()));if(child){run["child_token"]=tokenFacts(child);CloseHandle(child);}run["finished"]=p.waitForFinished(15000);run["exit_code"]=p.exitCode();run["output"]=QString::fromUtf8(p.readAll());runs.append(run);
 }
 all["runs"]=runs;std::cout<<QJsonDocument(all).toJson().toStdString();}
