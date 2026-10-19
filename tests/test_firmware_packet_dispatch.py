"""Execute the actual firmware parser/dispatch methods with host-side UART/FSM stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index("void CameraFirmware::"+name+"()")
    opening = source.index("{", start)
    depth = 1
    end = opening+1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PacketDispatch(unittest.TestCase):
    def test_back_to_back_messages_remain_intact_until_dispatched(self):
        source = (ROOT/"backend/src/CameraFirmware.cpp").read_text()
        harness = r'''
#include <cstdint>
#include <deque>
#include <vector>
#include <cassert>
#include "RingBuffer.h"
using byte=uint8_t;
constexpr int MAX_BUFFER_SIZE=256, DBG_INFO=2;
int debugLevel=0;
enum class BYTE_VALUE:byte { SOM=0xF0, EOM=0x55 };
enum class EVENT { PROCESS_MESSAGE, OTHER };
struct Uart {
 std::deque<int> bytes;
 int available() { return bytes.size(); }
 int read() { auto x=bytes.front(); bytes.pop_front(); return x; }
} Serial1;
struct { void println(const char*) {} } Serial;
class CameraFirmware;
struct State {
 std::vector<int> seen;
 void handleInput(CameraFirmware&, EVENT);
 void update(CameraFirmware&) {}
};
class CameraFirmware {
public:
 static RingBuffer<EVENT,20> mInputBuffer;
 static constexpr int INVALID_POSITION=-999;
 int lensToBodyBufferPosition=INVALID_POSITION, packetLength=0;
 byte lensToBodyBuffer[MAX_BUFFER_SIZE]{};
 bool mMessagePending=false;
 uint32_t mRxByteCount=0, mRxPacketCount=0;
 State state;
 State* mState=&state;
 void processByte();
 void updateFSM();
};
RingBuffer<EVENT,20> CameraFirmware::mInputBuffer;
void State::handleInput(CameraFirmware& firmware, EVENT e) {
 if(e==EVENT::PROCESS_MESSAGE) seen.push_back(firmware.lensToBodyBuffer[3]);
}
'''
        harness += method(source, "processByte")+"\n"+method(source, "updateFSM")
        harness += r'''
void packet(int type) { for (int b:{0xF0,7,0,type,0x12,0x34,0x55}) Serial1.bytes.push_back(b); }
int main() {
 CameraFirmware firmware;
 packet(1); packet(7); packet(12);
 // An unrelated queued event must not permit another reply to overwrite packet 1.
 firmware.mInputBuffer.push(EVENT::OTHER);
 firmware.processByte();
 assert(firmware.mMessagePending && firmware.lensToBodyBuffer[3]==1);
 assert(Serial1.available()==14);
 firmware.updateFSM();
 firmware.processByte();
 assert(firmware.lensToBodyBuffer[3]==1 && Serial1.available()==14);
 firmware.updateFSM();
 assert(!firmware.mMessagePending && firmware.state.seen==std::vector<int>({1}));
 firmware.processByte(); firmware.updateFSM();
 firmware.processByte(); firmware.updateFSM();
 assert(firmware.state.seen==std::vector<int>({1,7,12}));
 assert(Serial1.available()==0);
 // Malformed lengths and noise must not prevent subsequent valid replies.
 for(int b:{0x11,0xF0,0,0}) Serial1.bytes.push_back(b);
 packet(5); firmware.processByte(); firmware.updateFSM();
 assert(firmware.state.seen.back()==5);
 // Split packets wait for remaining bytes instead of dispatching partial data.
 for(int b:{0xF0,7,0,6}) Serial1.bytes.push_back(b);
 firmware.processByte(); assert(!firmware.mMessagePending);
 for(int b:{0x12,0x34,0x55}) Serial1.bytes.push_back(b);
 firmware.processByte(); firmware.updateFSM(); assert(firmware.state.seen.back()==6);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            cpp = Path(temporary)/"dispatch.cpp"
            binary = Path(temporary)/"dispatch"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++17", "-I"+str(ROOT/"backend/include"), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
