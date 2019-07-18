#include <winsock2.h>
#include <windows.h>
#include "iocpmodel.h"

using namespace std;


int main()
{
   IOCPMODEL miocp;

   miocp.LoadSocket();
   miocp.Start();
   while(1)
   {

   }
   return 0;
}
