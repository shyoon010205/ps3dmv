#pragma once
#include "holo.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
using holo::json;
namespace fs=std::filesystem;
namespace hmvalidation {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string timestamp() {
  SYSTEMTIME t;GetLocalTime(&t);std::ostringstream s;
  s<<std::setfill('0')<<std::setw(4)<<t.wYear<<std::setw(2)<<t.wMonth<<std::setw(2)<<t.wDay<<"_"
   <<std::setw(2)<<t.wHour<<std::setw(2)<<t.wMinute<<std::setw(2)<<t.wSecond<<"_"<<std::setw(3)<<t.wMilliseconds;
  return s.str();
}
struct Handle {
  HANDLE value=INVALID_HANDLE_VALUE;
  ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value!=nullptr)CloseHandle(value);}
};
std::wstring wide(const std::string& s) {
  int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
  check(n>0||s.empty(),"Invalid UTF-8 argument");
  std::wstring result(size_t(n),L'\0');
  if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),result.data(),n);
  return result;
}
std::wstring quote(const std::wstring& s) {
  std::wstring out=L"\"";unsigned slash=0;
  for(wchar_t c:s) {
    if(c==L'\\'){++slash;continue;}
    if(c==L'"'){out.append(slash*2+1,L'\\');out+=c;}
    else {out.append(slash,L'\\');out+=c;}
    slash=0;
  }
  out.append(slash*2,L'\\');out+=L'"';return out;
}
std::string sha256(const fs::path& path) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider failed");
  struct Cleanup {BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{algorithm,hash};
  check(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0,"SHA256 init failed");
  std::ifstream in(path,std::ios::binary);check(bool(in),"Cannot hash "+path.u8string());
  std::vector<unsigned char> buf(1024*1024);
  while(in) {
    in.read(reinterpret_cast<char*>(buf.data()),std::streamsize(buf.size()));
    if(in.gcount())check(BCryptHashData(hash,buf.data(),ULONG(in.gcount()),0)>=0,"SHA256 update failed");
  }
  check(in.eof(),"Hash read failed");
  std::array<unsigned char,32> digest{};
  check(BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0,"SHA256 finish failed");
  std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto c:digest)out<<std::setw(2)<<unsigned(c);
  return out.str();
}
json fileInfo(const fs::path& p) {
  check(fs::is_regular_file(p),"Missing output "+p.u8string());
  return {{"path",p.u8string()},{"bytes",fs::file_size(p)},{"sha256",sha256(p)}};
}
json compareFiles(const fs::path& a,const fs::path& b) {
  json result={{"a",fileInfo(a)},{"b",fileInfo(b)},{"identical",false}};
  check(fs::file_size(a)>0&&fs::file_size(b)>0,"Empty files cannot pass equivalence");
  std::ifstream x(a,std::ios::binary),y(b,std::ios::binary);
  std::vector<char> xa(1024*1024),ya(1024*1024);std::uint64_t offset=0;
  while(true) {
    x.read(xa.data(),std::streamsize(xa.size()));y.read(ya.data(),std::streamsize(ya.size()));
    const auto nx=x.gcount(),ny=y.gcount(),count=std::min(nx,ny);
    for(std::streamsize i=0;i<count;++i)if(xa[size_t(i)]!=ya[size_t(i)]) {
      result["first_different_byte"]=offset+std::uint64_t(i);return result;
    }
    if(nx!=ny){result["first_different_byte"]=offset+std::uint64_t(count);return result;}
    if(nx==0)break;offset+=std::uint64_t(nx);
  }
  check(x.eof()&&y.eof(),"Comparison read error");
  result["identical"]=true;return result;
}
json runProcess(const fs::path& executable,const std::vector<std::string>& args,const fs::path& log,const fs::path& cwd) {
  check(fs::is_regular_file(executable),"Executable missing: "+executable.u8string());
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
  Handle output{CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
  Handle input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
  check(output.value!=INVALID_HANDLE_VALUE&&input.value!=INVALID_HANDLE_VALUE,"Cannot open process log");
  STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;
  start.hStdOutput=output.value;start.hStdError=output.value;start.hStdInput=input.value;
  std::wstring command=quote(executable.wstring());for(const auto& arg:args)command+=L" "+quote(wide(arg));
  PROCESS_INFORMATION process{};const auto before=std::chrono::steady_clock::now();
  const BOOL started=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,cwd.c_str(),&start,&process);
  const DWORD startError=started?0:GetLastError();
  check(started!=0,"Cannot start "+executable.u8string()+"; Windows error "+std::to_string(startError));
  Handle ph{process.hProcess},th{process.hThread};
  check(WaitForSingleObject(ph.value,INFINITE)==WAIT_OBJECT_0,"Process wait failed");
  DWORD exitCode=0;check(GetExitCodeProcess(ph.value,&exitCode)!=0,"Cannot get process exit code");
  return {{"executable",executable.u8string()},{"arguments",args},{"exit_code",exitCode},
          {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count()},{"log",log.u8string()}};
}
void writeText(const fs::path& p,const std::string& text) {
  std::ofstream s(p,std::ios::binary|std::ios::trunc);check(bool(s),"Cannot write "+p.u8string());
  s<<text;s.close();check(bool(s),"Write failed "+p.u8string());
}
}
