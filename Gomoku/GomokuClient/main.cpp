
#include "GomokuClinet.h"
int main() {
   
    GomokuClient client("127.0.0.1", 8888);
   
   // client.init();
    client.start();
    return 0;
}
