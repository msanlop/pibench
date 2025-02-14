#ifndef ART_EPOCHE_H
#define ART_EPOCHE_H

#include <array>
#include <atomic>
#include <cassert>
#include <iostream>

#include "tbb/combinable.h"
#include "tbb/enumerable_thread_specific.h"

namespace ART {

struct LabelDelete {
  std::array<void *, 32> nodes;
  uint64_t epoche;
  std::size_t nodesCount;
  LabelDelete *next;
};

class DeletionList {
  LabelDelete *headDeletionList = nullptr;
  LabelDelete *freeLabelDeletes = nullptr;
  std::size_t deletitionListCount = 0;

 public:
  std::atomic<uint64_t> localEpoche;
  size_t thresholdCounter{0};

  ~DeletionList();
  LabelDelete *head();

  void add(void *n, uint64_t globalEpoch);

  void remove(LabelDelete *label, LabelDelete *prev);

  std::size_t size();

  std::uint64_t deleted = 0;
  std::uint64_t added = 0;
};

class Epoche;
class EpocheGuard;

class ThreadInfo {
  friend class Epoche;
  friend class EpocheGuard;
  Epoche &epoche;
  DeletionList &deletionList;

  DeletionList &getDeletionList() const;

 public:
  ThreadInfo(Epoche &epoche);

  ThreadInfo(const ThreadInfo &ti) : epoche(ti.epoche), deletionList(ti.deletionList) {}

  Epoche &getEpoche() const;
};

class Epoche {
  friend class ThreadInfo;
  std::atomic<uint64_t> currentEpoche{0};

  tbb::enumerable_thread_specific<DeletionList> deletionLists;

  size_t startGCThreshhold;

 public:
  Epoche(size_t startGCThreshhold) : startGCThreshhold(startGCThreshhold) {}

  ~Epoche();

  void enterEpoche(ThreadInfo &epocheInfo);

  void markNodeForDeletion(void *n, ThreadInfo &epocheInfo);

  void exitEpoche(ThreadInfo &info);

  void exitEpocheAndCleanup(ThreadInfo &info);

  void showDeleteRatio();
};

class EpocheGuard {
  ThreadInfo &threadEpocheInfo;

 public:
  EpocheGuard(ThreadInfo &threadEpocheInfo) : threadEpocheInfo(threadEpocheInfo) {
    threadEpocheInfo.getEpoche().enterEpoche(threadEpocheInfo);
  }

  ~EpocheGuard() { threadEpocheInfo.getEpoche().exitEpocheAndCleanup(threadEpocheInfo); }
};

class EpocheGuardReadonly {
  ThreadInfo &threadEpocheInfo;

 public:
  EpocheGuardReadonly(ThreadInfo &threadEpocheInfo) : threadEpocheInfo(threadEpocheInfo) {
    threadEpocheInfo.getEpoche().enterEpoche(threadEpocheInfo);
  }

  ~EpocheGuardReadonly() { threadEpocheInfo.getEpoche().exitEpoche(threadEpocheInfo); }
};

inline DeletionList::~DeletionList() {
  assert(deletitionListCount == 0 && headDeletionList == nullptr);
  LabelDelete *cur = nullptr, *next = freeLabelDeletes;
  while (next != nullptr) {
    cur = next;
    next = cur->next;
    delete cur;
  }
  freeLabelDeletes = nullptr;
}

inline std::size_t DeletionList::size() { return deletitionListCount; }

inline void DeletionList::remove(LabelDelete *label, LabelDelete *prev) {
  if (prev == nullptr) {
    headDeletionList = label->next;
  } else {
    prev->next = label->next;
  }
  deletitionListCount -= label->nodesCount;

  label->next = freeLabelDeletes;
  freeLabelDeletes = label;
  deleted += label->nodesCount;
}

inline void DeletionList::add(void *n, uint64_t globalEpoch) {
  deletitionListCount++;
  LabelDelete *label;
  if (headDeletionList != nullptr &&
      headDeletionList->nodesCount < headDeletionList->nodes.size()) {
    label = headDeletionList;
  } else {
    if (freeLabelDeletes != nullptr) {
      label = freeLabelDeletes;
      freeLabelDeletes = freeLabelDeletes->next;
    } else {
      label = new LabelDelete();
    }
    label->nodesCount = 0;
    label->next = headDeletionList;
    headDeletionList = label;
  }
  label->nodes[label->nodesCount] = n;
  label->nodesCount++;
  label->epoche = globalEpoch;

  added++;
}

inline LabelDelete *DeletionList::head() { return headDeletionList; }

inline void Epoche::enterEpoche(ThreadInfo &epocheInfo) {
  unsigned long curEpoche = currentEpoche.load(std::memory_order_relaxed);
  epocheInfo.getDeletionList().localEpoche.store(curEpoche, std::memory_order_release);
}

inline void Epoche::markNodeForDeletion(void *n, ThreadInfo &epocheInfo) {
  epocheInfo.getDeletionList().add(n, currentEpoche.load());
  epocheInfo.getDeletionList().thresholdCounter++;
}

inline void Epoche::exitEpoche(ThreadInfo &epocheInfo) {
  epocheInfo.getDeletionList().localEpoche.store(std::numeric_limits<uint64_t>::max());
}

inline void Epoche::exitEpocheAndCleanup(ThreadInfo &epocheInfo) {
  DeletionList &deletionList = epocheInfo.getDeletionList();
  if ((deletionList.thresholdCounter & (64 - 1)) == 1) {
    currentEpoche++;
  }
  if (deletionList.thresholdCounter > startGCThreshhold) {
    deletionList.localEpoche.store(std::numeric_limits<uint64_t>::max());
    if (deletionList.size() == 0) {
      deletionList.thresholdCounter = 0;
      return;
    }

    uint64_t oldestEpoche = std::numeric_limits<uint64_t>::max();
    for (auto &epoche : deletionLists) {
      auto e = epoche.localEpoche.load();
      if (e < oldestEpoche) {
        oldestEpoche = e;
      }
    }

    LabelDelete *cur = deletionList.head(), *next, *prev = nullptr;
    while (cur != nullptr) {
      next = cur->next;

      if (cur->epoche < oldestEpoche) {
        for (std::size_t i = 0; i < cur->nodesCount; ++i) {
          operator delete(cur->nodes[i]);
        }
        deletionList.remove(cur, prev);
      } else {
        prev = cur;
      }
      cur = next;
    }
    deletionList.thresholdCounter = 0;
  } else {
    deletionList.localEpoche.store(std::numeric_limits<uint64_t>::max());
  }
}

inline Epoche::~Epoche() {
  uint64_t oldestEpoche = std::numeric_limits<uint64_t>::max();
  for (auto &epoche : deletionLists) {
    auto e = epoche.localEpoche.load();
    if (e < oldestEpoche) {
      oldestEpoche = e;
    }
  }
  for (auto &d : deletionLists) {
    LabelDelete *cur = d.head(), *next, *prev = nullptr;
    while (cur != nullptr) {
      next = cur->next;

      assert(cur->epoche < oldestEpoche);
      for (std::size_t i = 0; i < cur->nodesCount; ++i) {
        operator delete(cur->nodes[i]);
      }
      d.remove(cur, prev);
      cur = next;
    }
  }
}

inline void Epoche::showDeleteRatio() {
  for (auto &d : deletionLists) {
    std::cout << "deleted " << d.deleted << " of " << d.added << std::endl;
  }
}

inline ThreadInfo::ThreadInfo(Epoche &epoche)
    : epoche(epoche), deletionList(epoche.deletionLists.local()) {}

inline DeletionList &ThreadInfo::getDeletionList() const { return deletionList; }

inline Epoche &ThreadInfo::getEpoche() const { return epoche; }

}  // namespace ART

#endif  // ART_EPOCHE_H
