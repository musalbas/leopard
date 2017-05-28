/*
    Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Leopard-RS nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "../LeopardCommon.h"
#include "../LeopardFF8.h"
#include "../LeopardFF16.h"
#include "../leopard.h"

#include <memory>
#include <vector>
#include <iostream>
#include <string>
using namespace std;

//#define TEST_DATA_ALL_SAME
//#define TEST_LOSE_FIRST_K_PACKETS


//------------------------------------------------------------------------------
// Windows

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN

    #ifndef _WINSOCKAPI_
        #define DID_DEFINE_WINSOCKAPI
        #define _WINSOCKAPI_
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601 /* Windows 7+ */
    #endif

    #include <windows.h>
#endif

#ifdef DID_DEFINE_WINSOCKAPI
    #undef _WINSOCKAPI_
    #undef DID_DEFINE_WINSOCKAPI
#endif


//------------------------------------------------------------------------------
// Threads

static bool SetCurrentThreadPriority()
{
#ifdef _WIN32
    return 0 != ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    return -1 != nice(2);
#endif
}


//------------------------------------------------------------------------------
// Timing

static uint64_t GetTimeUsec()
{
#ifdef _WIN32
    LARGE_INTEGER timeStamp = {};
    if (!::QueryPerformanceCounter(&timeStamp))
        return 0;
    static double PerfFrequencyInverse = 0.;
    if (PerfFrequencyInverse == 0.)
    {
        LARGE_INTEGER freq = {};
        if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return 0;
        PerfFrequencyInverse = 1000000. / (double)freq.QuadPart;
    }
    return (uint64_t)(PerfFrequencyInverse * timeStamp.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return 1000000 * tv.tv_sec + tv.tv_usec;
#endif // _WIN32
}


//------------------------------------------------------------------------------
// PCG PRNG
// From http://www.pcg-random.org/

class PCGRandom
{
public:
    inline void Seed(uint64_t y, uint64_t x = 0)
    {
        State = 0;
        Inc = (y << 1u) | 1u;
        Next();
        State += x;
        Next();
    }

    inline uint32_t Next()
    {
        const uint64_t oldstate = State;
        State = oldstate * UINT64_C(6364136223846793005) + Inc;
        const uint32_t xorshifted = (uint32_t)(((oldstate >> 18) ^ oldstate) >> 27);
        const uint32_t rot = oldstate >> 59;
        return (xorshifted >> rot) | (xorshifted << ((uint32_t)(-(int32_t)rot) & 31));
    }

    uint64_t State = 0, Inc = 0;
};


//------------------------------------------------------------------------------
// Self-Checking Packet

static void WriteRandomSelfCheckingPacket(PCGRandom& prng, void* packet, unsigned bytes)
{
    uint8_t* buffer = (uint8_t*)packet;
#ifdef TEST_DATA_ALL_SAME
    if (bytes != 0)
#else
    if (bytes < 16)
#endif
    {
        LEO_DEBUG_ASSERT(bytes >= 2);
        buffer[0] = (uint8_t)prng.Next();
        for (unsigned i = 1; i < bytes; ++i)
        {
            buffer[i] = buffer[0];
        }
    }
    else
    {
        uint32_t crc = bytes;
        *(uint32_t*)(buffer + 4) = bytes;
        for (unsigned i = 8; i < bytes; ++i)
        {
            uint8_t v = (uint8_t)prng.Next();
            buffer[i] = v;
            crc = (crc << 3) | (crc >> (32 - 3));
            crc += v;
        }
        *(uint32_t*)buffer = crc;
    }
}

static bool CheckPacket(const void* packet, unsigned bytes)
{
    uint8_t* buffer = (uint8_t*)packet;
#ifdef TEST_DATA_ALL_SAME
    if (bytes != 0)
#else
    if (bytes < 16)
#endif
    {
        if (bytes < 2)
            return false;

        uint8_t v = buffer[0];
        for (unsigned i = 1; i < bytes; ++i)
        {
            if (buffer[i] != v)
                return false;
        }
    }
    else
    {
        uint32_t crc = bytes;
        uint32_t readBytes = *(uint32_t*)(buffer + 4);
        if (readBytes != bytes)
            return false;
        for (unsigned i = 8; i < bytes; ++i)
        {
            uint8_t v = buffer[i];
            crc = (crc << 3) | (crc >> (32 - 3));
            crc += v;
        }
        uint32_t readCRC = *(uint32_t*)buffer;
        if (readCRC != crc)
            return false;
    }
    return true;
}


//------------------------------------------------------------------------------
// FunctionTimer

class FunctionTimer
{
public:
    FunctionTimer(const std::string& name)
    {
        FunctionName = name;
    }
    void BeginCall()
    {
        LEO_DEBUG_ASSERT(t0 == 0);
        t0 = GetTimeUsec();
    }
    void EndCall()
    {
        LEO_DEBUG_ASSERT(t0 != 0);
        uint64_t t1 = GetTimeUsec();
        ++Invokations;
        TotalUsec += t1 - t0;
        t0 = 0;
    }
    void Reset()
    {
        LEO_DEBUG_ASSERT(t0 == 0);
        t0 = 0;
        Invokations = 0;
        TotalUsec = 0;
    }
    void Print(unsigned trials)
    {
        cout << FunctionName << " called " << Invokations / (float)trials << " times per trial. " << TotalUsec / (double)Invokations << " usec avg. " << TotalUsec / (float)trials << " usec for each of " << trials << " trials" << endl;
    }

    uint64_t t0 = 0;
    uint64_t Invokations = 0;
    uint64_t TotalUsec = 0;
    std::string FunctionName;
};


//------------------------------------------------------------------------------
// Utility: Deck Shuffling function

/*
    Given a PRNG, generate a deck of cards in a random order.
    The deck will contain elements with values between 0 and count - 1.
*/

static void ShuffleDeck16(PCGRandom &prng, uint16_t * LEO_RESTRICT deck, uint32_t count)
{
    deck[0] = 0;

    // If we can unroll 4 times,
    if (count <= 256)
    {
        for (uint32_t ii = 1;;)
        {
            uint32_t jj, rv = prng.Next();

            // 8-bit unroll
            switch (count - ii)
            {
            default:
                jj = (uint8_t)rv % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                jj = (uint8_t)(rv >> 8) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                jj = (uint8_t)(rv >> 16) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                jj = (uint8_t)(rv >> 24) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                break;

            case 3:
                jj = (uint8_t)rv % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
            case 2:
                jj = (uint8_t)(rv >> 8) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
            case 1:
                jj = (uint8_t)(rv >> 16) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
            case 0:
                return;
            }
        }
    }
    else
    {
        // For each deck entry,
        for (uint32_t ii = 1;;)
        {
            uint32_t jj, rv = prng.Next();

            // 16-bit unroll
            switch (count - ii)
            {
            default:
                jj = (uint16_t)rv % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                jj = (uint16_t)(rv >> 16) % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
                ++ii;
                break;

            case 1:
                jj = (uint16_t)rv % ii;
                deck[ii] = deck[jj];
                deck[jj] = ii;
            case 0:
                return;
            }
        }
    }
}


//------------------------------------------------------------------------------
// SIMD-Safe Aligned Memory Allocations

static const unsigned kAlignmentBytes = LEO_ALIGN_BYTES;

LEO_FORCE_INLINE unsigned NextAlignedOffset(unsigned offset)
{
    return (offset + kAlignmentBytes - 1) & ~(kAlignmentBytes - 1);
}

static LEO_FORCE_INLINE uint8_t* SIMDSafeAllocate(size_t size)
{
    uint8_t* data = (uint8_t*)calloc(1, kAlignmentBytes + size);
    if (!data)
        return nullptr;
    unsigned offset = (unsigned)((uintptr_t)data % kAlignmentBytes);
    data += kAlignmentBytes - offset;
    data[-1] = (uint8_t)offset;
    return data;
}

static LEO_FORCE_INLINE void SIMDSafeFree(void* ptr)
{
    if (!ptr)
        return;
    uint8_t* data = (uint8_t*)ptr;
    unsigned offset = data[-1];
    if (offset >= kAlignmentBytes)
    {
        LEO_DEBUG_BREAK; // Should never happen
        return;
    }
    data -= kAlignmentBytes - offset;
    free(data);
}


//------------------------------------------------------------------------------
// Tests

struct TestParameters
{
#ifdef LEO_HAS_FF16
    unsigned original_count = 1000; // under 65536
    unsigned recovery_count = 100; // under 65536 - original_count
#else
    unsigned original_count = 100; // under 65536
    unsigned recovery_count = 10; // under 65536 - original_count
#endif
    unsigned buffer_bytes = 64000; // multiple of 64 bytes
    unsigned loss_count = 10; // some fraction of original_count
    unsigned seed = 0;
    bool multithreaded = true;
};

static void BasicTest(const TestParameters& params)
{
    static const unsigned kTrials = 10;

    std::vector<uint8_t*> original_data(params.original_count);

    const unsigned encode_work_count = leo_encode_work_count(params.original_count, params.recovery_count);
    const unsigned decode_work_count = leo_decode_work_count(params.original_count, params.recovery_count);

    std::vector<uint8_t*> encode_work_data(encode_work_count);
    std::vector<uint8_t*> decode_work_data(decode_work_count);

    FunctionTimer t_mem_alloc("memory_allocation");
    FunctionTimer t_leo_encode("leo_encode");
    FunctionTimer t_leo_decode("leo_decode");
    FunctionTimer t_mem_free("memory_free");

    const uint64_t total_bytes = (uint64_t)params.buffer_bytes * params.original_count;

    for (unsigned trial = 0; trial < kTrials; ++trial)
    {
        // Allocate memory:

        t_mem_alloc.BeginCall();
        for (unsigned i = 0, count = params.original_count; i < count; ++i)
            original_data[i] = SIMDSafeAllocate(params.buffer_bytes);
        for (unsigned i = 0, count = encode_work_count; i < count; ++i)
            encode_work_data[i] = SIMDSafeAllocate(params.buffer_bytes);
        for (unsigned i = 0, count = decode_work_count; i < count; ++i)
            decode_work_data[i] = SIMDSafeAllocate(params.buffer_bytes);
        t_mem_alloc.EndCall();

        // Generate data:

        PCGRandom prng;
        prng.Seed(params.seed, trial);

        for (unsigned i = 0; i < params.original_count; ++i)
            WriteRandomSelfCheckingPacket(prng, original_data[i], params.buffer_bytes);

        // Encode:

        t_leo_encode.BeginCall();
        LeopardResult encodeResult = leo_encode(
            params.buffer_bytes,
            params.original_count,
            params.recovery_count,
            encode_work_count,
            (void**)&original_data[0],
            (void**)&encode_work_data[0], // recovery data written here
            params.multithreaded ? LeopardFlags_Multithreaded : LeopardFlags_Defaults
        );
        t_leo_encode.EndCall();

        if (encodeResult != Leopard_Success)
        {
            cout << "Error: Leopard encode failed with result=" << encodeResult << endl;
            LEO_DEBUG_BREAK;
            return;
        }

        // Lose random original data:

        std::vector<uint16_t> original_losses(params.original_count);
        ShuffleDeck16(prng, &original_losses[0], params.original_count);

        for (unsigned i = 0, count = params.loss_count; i < count; ++i)
        {
            const unsigned loss_index = original_losses[i];
            SIMDSafeFree(original_data[loss_index]);
            original_data[loss_index] = nullptr;
        }

        // Lose random recovery data:

        const unsigned recovery_loss_count = params.recovery_count - params.loss_count;

        std::vector<uint16_t> recovery_losses(params.recovery_count);
        ShuffleDeck16(prng, &recovery_losses[0], params.recovery_count);

        for (unsigned i = 0, count = recovery_loss_count; i < count; ++i)
        {
            const unsigned loss_index = recovery_losses[i];
            SIMDSafeFree(encode_work_data[loss_index]);
            encode_work_data[loss_index] = nullptr;
        }

        // Decode:

        t_leo_decode.BeginCall();
        LeopardResult decodeResult = leo_decode(
            params.buffer_bytes,
            params.original_count,
            params.recovery_count,
            decode_work_count,
            (void**)&original_data[0],
            (void**)&encode_work_data[0],
            (void**)&decode_work_data[0],
            params.multithreaded ? LeopardFlags_Multithreaded : LeopardFlags_Defaults);
        t_leo_decode.EndCall();

        if (decodeResult != Leopard_Success)
        {
            cout << "Error: Leopard decode failed with result=" << decodeResult << endl;
            LEO_DEBUG_BREAK;
            return;
        }

#if 0
        for (unsigned i = 0; i < params.original_count; ++i)
        {
            if (!original_data[i])
            {
                cout << "Checking " << i << endl;
                if (!CheckPacket(decode_work_data[i], params.buffer_bytes))
                {
                    cout << "Error: Data was corrupted" << endl;
                    LEO_DEBUG_BREAK;
                    return;
                }
            }
        }
#endif

        // Free memory:

        t_mem_free.BeginCall();
        for (unsigned i = 0, count = params.original_count; i < count; ++i)
            SIMDSafeFree(original_data[i]);
        for (unsigned i = 0, count = encode_work_count; i < count; ++i)
            SIMDSafeFree(encode_work_data[i]);
        for (unsigned i = 0, count = decode_work_count; i < count; ++i)
            SIMDSafeFree(decode_work_data[i]);
        t_mem_free.EndCall();
    }

    t_mem_alloc.Print(kTrials);
    t_leo_encode.Print(kTrials);
    t_leo_decode.Print(kTrials);
    t_mem_free.Print(kTrials);

    float encode_input_MBPS = total_bytes * kTrials / (float)(t_leo_encode.TotalUsec);
    float encode_output_MBPS = params.buffer_bytes * (uint64_t)params.recovery_count * kTrials / (float)(t_leo_encode.TotalUsec);
    float decode_input_MBPS = total_bytes * kTrials / (float)(t_leo_decode.TotalUsec);
    float decode_output_MBPS = params.buffer_bytes * (uint64_t)params.loss_count * kTrials / (float)(t_leo_decode.TotalUsec);

    cout << "Leopard Encoder(" << total_bytes / 1000000.f << " MB in " << params.original_count << " pieces, " << params.loss_count << " losses): Input=" << encode_input_MBPS << " MB/s, Output=" << encode_output_MBPS << " MB/s" << endl;
    cout << "Leopard Decoder(" << total_bytes / 1000000.f << " MB in " << params.original_count << " pieces, " << params.loss_count << " losses): Input=" << decode_input_MBPS << " MB/s, Output=" << decode_output_MBPS << " MB/s" << endl << endl;
}


//------------------------------------------------------------------------------
// Parallel XOR Benchmark

// Demonstrate about 10% performance boost by doing parallel rows for XORs
void ParallelXORBenchmark()
{
    FunctionTimer t_1("xor_mem");
    FunctionTimer t_4("xor_mem4");

    static const unsigned buffer_bytes = 4096;
    static const unsigned buffer_count = 1024;

    uint8_t* buffers_x[buffer_count] = {};
    uint8_t* buffers_y[buffer_count] = {};

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        buffers_x[i] = SIMDSafeAllocate(buffer_bytes);
        buffers_y[i] = SIMDSafeAllocate(buffer_bytes);
    }

    static const unsigned iteration_count = 1000;

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        t_1.BeginCall();
        for (unsigned j = 0; j < buffer_count; ++j)
            leopard::xor_mem(
                buffers_x[j], buffers_y[j],
                buffer_bytes);
        t_1.EndCall();
    }

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        t_4.BeginCall();
        for (unsigned j = 0; j < buffer_count; j += 4)
            leopard::xor_mem4(
                buffers_x[j], buffers_y[j],
                buffers_x[j + 1], buffers_y[j + 1],
                buffers_x[j + 2], buffers_y[j + 2],
                buffers_x[j + 3], buffers_y[j + 3],
                buffer_bytes);
        t_4.EndCall();
    }

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        SIMDSafeFree(buffers_x[i]);
        SIMDSafeFree(buffers_y[i]);
    }

    t_1.Print(iteration_count);
    t_4.Print(iteration_count);
}


//------------------------------------------------------------------------------
// Parallel Butterfly8 Benchmark

#ifdef LEO_HAS_FF8

// Demonstrate performance boost by doing parallel rows for Butterfly8s
void ParallelButterfly8Benchmark()
{
    FunctionTimer t_1("8-bit fft_butterfly");
    FunctionTimer t_4("8-bit fft_butterfly4");

    static const unsigned buffer_bytes = 4096;
    static const unsigned buffer_count = 1024;

    uint8_t* buffers_x[buffer_count] = {};
    uint8_t* buffers_y[buffer_count] = {};

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        buffers_x[i] = SIMDSafeAllocate(buffer_bytes);
        buffers_y[i] = SIMDSafeAllocate(buffer_bytes);
    }

    static const unsigned iteration_count = 1000;

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        leopard::ff8::ffe_t m = (leopard::ff8::ffe_t)(i + 2);

        t_1.BeginCall();
        for (unsigned j = 0; j < buffer_count; ++j)
            leopard::ff8::fft_butterfly(
                buffers_x[j], buffers_y[j],
                m,
                buffer_bytes);
        t_1.EndCall();
    }

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        leopard::ff8::ffe_t m = (leopard::ff8::ffe_t)(i + 2);

        t_4.BeginCall();
        for (unsigned j = 0; j < buffer_count; j += 4)
            leopard::ff8::fft_butterfly4(
                buffers_x[j], buffers_y[j],
                buffers_x[j + 1], buffers_y[j + 1],
                buffers_x[j + 2], buffers_y[j + 2],
                buffers_x[j + 3], buffers_y[j + 3],
                m,
                buffer_bytes);
        t_4.EndCall();
    }

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        SIMDSafeFree(buffers_x[i]);
        SIMDSafeFree(buffers_y[i]);
    }

    t_1.Print(iteration_count);
    t_4.Print(iteration_count);
}

#endif // LEO_HAS_FF8


//------------------------------------------------------------------------------
// Parallel Butterfly16 Benchmark

#ifdef LEO_HAS_FF16

// Demonstrate performance boost by doing parallel rows for Butterfly16s
void ParallelButterfly16Benchmark()
{
    FunctionTimer t_1("16-bit fft_butterfly");
    FunctionTimer t_4("16-bit fft_butterfly4");

    static const unsigned buffer_bytes = 4096;
    static const unsigned buffer_count = 1024;

    uint8_t* buffers_x[buffer_count] = {};
    uint8_t* buffers_y[buffer_count] = {};

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        buffers_x[i] = SIMDSafeAllocate(buffer_bytes);
        buffers_y[i] = SIMDSafeAllocate(buffer_bytes);
    }

    static const unsigned iteration_count = 100;

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        leopard::ff16::ffe_t m = (leopard::ff16::ffe_t)(i + 2);

        t_1.BeginCall();
        for (unsigned j = 0; j < buffer_count; ++j)
            leopard::ff16::fft_butterfly(
                buffers_x[j], buffers_y[j],
                m,
                buffer_bytes);
        t_1.EndCall();
    }

    for (unsigned i = 0; i < iteration_count; ++i)
    {
        leopard::ff16::ffe_t m = (leopard::ff16::ffe_t)(i + 2);

        t_4.BeginCall();
        for (unsigned j = 0; j < buffer_count; j += 4)
            leopard::ff16::fft_butterfly4(
                buffers_x[j], buffers_y[j],
                buffers_x[j + 1], buffers_y[j + 1],
                buffers_x[j + 2], buffers_y[j + 2],
                buffers_x[j + 3], buffers_y[j + 3],
                m,
                buffer_bytes);
        t_4.EndCall();
    }

    for (unsigned i = 0; i < buffer_count; ++i)
    {
        SIMDSafeFree(buffers_x[i]);
        SIMDSafeFree(buffers_y[i]);
    }

    t_1.Print(iteration_count);
    t_4.Print(iteration_count);
}

#endif // LEO_HAS_FF8


//------------------------------------------------------------------------------
// Entrypoint

int main(int argc, char **argv)
{
    SetCurrentThreadPriority();

    FunctionTimer t_leo_init("leo_init");

    t_leo_init.BeginCall();
    if (0 != leo_init())
    {
        cout << "Failed to initialize" << endl;
        return -1;
    }
    t_leo_init.EndCall();
    t_leo_init.Print(1);

    ParallelXORBenchmark();
#ifdef LEO_HAS_FF8
    ParallelButterfly8Benchmark();
#endif // LEO_HAS_FF8
#ifdef LEO_HAS_FF16
    ParallelButterfly16Benchmark();
#endif // LEO_HAS_FF16

    TestParameters params;

    if (argc >= 2)
        params.original_count = atoi(argv[1]);
    if (argc >= 3)
        params.recovery_count = atoi(argv[2]);
    if (argc >= 4)
        params.buffer_bytes = atoi(argv[3]);
    if (argc >= 5)
        params.loss_count = atoi(argv[4]);
    if (argc >= 6)
        params.multithreaded = (atoi(argv[5]) != 0);

    cout << "Parameters: [original count=" << params.original_count << "] [recovery count=" << params.recovery_count << "] [buffer bytes=" << params.buffer_bytes << "] [loss count=" << params.loss_count << "] [random seed=" << params.seed << "]" << endl;

    BasicTest(params);

    getchar();

    return 0;
}
