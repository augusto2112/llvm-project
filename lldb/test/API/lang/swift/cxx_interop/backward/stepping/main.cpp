#include "swift-types.h"

using namespace a;

int testFunc() {
  swiftFunc(); // Break here for func
  return 0;
}


int main() {
  testFunc();
  return 0;
}
