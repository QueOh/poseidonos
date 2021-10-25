/*
 *   BSD LICENSE
 *   Copyright (c) 2021 Samsung Electronics Corporation
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Samsung Electronics Corporation nor the names of
 *       its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "src/allocator/stripe/stripe.h"

#include <string>

#include "src/array_mgmt/array_manager.h"
#include "src/include/branch_prediction.h"
#include "src/include/meta_const.h"
#include "src/include/pos_event_id.h"
#include "src/spdk_wrapper/accel_engine_api.h"
#include "src/volume/volume_list.h"
#include "src/mapper/i_reversemap.h"
#include "src/mapper/reversemap/reverse_map.h"

namespace pos
{
Stripe::Stripe(ReverseMapPack* rev, IReverseMap* revMapMan, bool withDataBuffer_, uint32_t numBlksPerStripe)
: asTailArrayIdx(UINT32_MAX),
  vsid(UINT32_MAX),
  wbLsid(UINT32_MAX),
  userLsid(UINT32_MAX),
  revMapPack(rev),
  finished(true),
  remaining(0),
  referenceCount(0),
  totalBlksPerUserStripe(numBlksPerStripe), // for UT
  withDataBuffer(withDataBuffer_),
  iReverseMap(revMapMan)
{
    if (withDataBuffer == false)
    {
        POS_TRACE_INFO(EID(GC_STRIPE_ALLOCATED), "Gc stripe is allocated!");
    }
    flushIo = nullptr;
}

Stripe::Stripe(IReverseMap* revMapMan, bool withDataBuffer_, uint32_t numBlksPerStripe)
: Stripe(nullptr, revMapMan, withDataBuffer_, numBlksPerStripe)
{
}

Stripe::~Stripe(void)
{
    if (withDataBuffer == false)
    {
        if (revMapPack != nullptr)
        {
            delete revMapPack;
            revMapPack = nullptr;
        }
    }
}

void
Stripe::UpdateVictimVsa(uint32_t offset, VirtualBlkAddr vsa, BlkAddr rba, uint32_t volumeId)
{
    oldVsaList[offset] = vsa;
}

VirtualBlkAddr
Stripe::GetVictimVsa(uint32_t offset)
{
    return oldVsaList[offset];
}

void
Stripe::Assign(StripeId vsid_, StripeId lsid_, ASTailArrayIdx tailArrayIdx_)
{
    vsid = vsid_;
    wbLsid = lsid_;
    asTailArrayIdx = tailArrayIdx_;
    oldVsaList.assign(totalBlksPerUserStripe, UNMAP_VSA);
    remaining.store(totalBlksPerUserStripe, memory_order_release);
    finished = false;
    if (withDataBuffer == false)
    {
        revMapPack = iReverseMap->AllocReverseMapPack(vsid);
    }
    userLsid = vsid; // Future work: Allocate userLsid in Flush submission
}

bool
Stripe::IsOkToFree(void)
{
    bool isOkToFree = (0 == referenceCount);
    return isOkToFree;
}

void
Stripe::Derefer(uint32_t blockCount)
{
    if (unlikely(blockCount > referenceCount))
    {
        POS_EVENT_ID eventId =
            POS_EVENT_ID::ALLOCATOR_WRONG_STRIPE_REFERENCE_COUNT;
        POS_TRACE_ERROR((int)eventId, "Wrong stripe reference count");
        referenceCount = 0;
        return;
    }
    referenceCount -= blockCount;
}

void
Stripe::Refer(void)
{
    referenceCount++;
}

StripeId
Stripe::GetWbLsid(void)
{
    return wbLsid;
}

void
Stripe::SetWbLsid(StripeId wbAreaLsid)
{
    wbLsid = wbAreaLsid;
}

StripeId
Stripe::GetUserLsid(void)
{
    return userLsid;
}

void
Stripe::SetUserLsid(StripeId userAreaLsid)
{
    userLsid = userAreaLsid;
}

StripeId
Stripe::GetVsid(void)
{
    return vsid;
}

void
Stripe::SetVsid(StripeId virtsid)
{
    vsid = virtsid;
}

uint32_t
Stripe::GetAsTailArrayIdx(void)
{
    return asTailArrayIdx;
}

void
Stripe::UpdateReverseMapEntry(uint32_t offset, BlkAddr rba, uint32_t volumeId)
{
    if (rba != INVALID_RBA && volumeId >= MAX_VOLUME_COUNT)
    {
        throw POS_EVENT_ID::STRIPE_INVALID_VOLUME_ID;
    }
    iReverseMap->UpdateReverseMapEntry(revMapPack, wbLsid, offset, rba, volumeId);
}

std::tuple<BlkAddr, uint32_t>
Stripe::GetReverseMapEntry(uint32_t offset)
{
    return iReverseMap->GetReverseMapEntry(revMapPack, wbLsid, offset);
}

int
Stripe::Flush(EventSmartPtr callback)
{
    if (likely(revMapPack != nullptr))
    {
        return iReverseMap->Flush(revMapPack, UNMAP_STRIPE, this, vsid, callback);
    }
    else
    {
        POS_TRACE_ERROR(EID(ALLOCATOR_STRIPE_WITHOUT_REVERSEMAP),
            "Stripe object for wbLsid:{} is not linked to reversemap but tried to Flush, GcStripe:{}",
            wbLsid, (!withDataBuffer));
        return -EID(ALLOCATOR_STRIPE_WITHOUT_REVERSEMAP);
    }
}

DataBufferIter
Stripe::DataBufferBegin(void)
{
    assert(withDataBuffer == true);
    return dataBuffer.begin();
}

DataBufferIter
Stripe::DataBufferEnd(void)
{
    assert(withDataBuffer == true);
    return dataBuffer.end();
}

void
Stripe::AddDataBuffer(void* buf)
{
    assert(withDataBuffer == true);
    dataBuffer.push_back(buf);
}

uint32_t
Stripe::GetBlksRemaining(void)
{
    return remaining.load(memory_order_acquire);
}

uint32_t
Stripe::DecreseBlksRemaining(uint32_t amount)
{
    uint32_t remainingBlocksAfterDecrease =
        remaining.fetch_sub(amount, memory_order_acq_rel) - amount;
    return remainingBlocksAfterDecrease;
}

bool
Stripe::IsFinished(void)
{
    return (finished == true);
}

void
Stripe::SetFinished(bool state)
{
    std::unique_lock<std::mutex> lock(flushIoUpdate);
    finished = state;

    if (flushIo != nullptr)
    {
        flushIo->DecreaseStripeCnt();
        flushIo = nullptr;
    }
}

void
Stripe::UpdateFlushIo(FlushIoSmartPtr flushIo)
{
    std::unique_lock<std::mutex> lock(flushIoUpdate);

    if (finished == false)
    {
        flushIo->IncreaseStripeCnt();
        this->flushIo = flushIo;
    }
}

} // namespace pos
