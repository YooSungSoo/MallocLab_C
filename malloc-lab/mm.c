/*
 * mm-implicit.c - Implicit free list allocator (first-fit)
 *
 * 개요(High-level):
 *   - 8바이트 정렬을 보장하는 힙 관리자입니다.
 *   - 각 블록은 [Header | Payload | (Footer)] 형태로 저장됩니다.
 *     * Header/Footer: 4바이트(word)로, (블록 전체 크기 | 할당 비트) 를 담습니다.
 *     * 최소 블록 크기: 16바이트(헤더 4 + 풋터 4 + 최소 페이로드 8)
 *   - 힙의 앞뒤에 Prologue(할당된 최소 가드 블록) / Epilogue(크기 0, 할당) 가드 블록을 둡니다.
 *   - 탐색은 first-fit, 배치 시 필요하면 분할(splitting)합니다.
 *   - free 시 즉시 인접 free 블록과 병합(coalescing)합니다.
 *   - realloc 은 새로 할당 후 데이터 복사, 기존 블록 free 로 단순 구현했습니다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    /* Team name */
    "Jungle Team 12",
    /* First member's full name */
    "Yoo SungSoo",
    /* First member's email address */
    "elcane2@naver.com",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

/* ====== 상수/매크로 정의 ====== */

/* 단위 크기들 */
#define WSIZE       4             /* word size: 헤더/풋터 단위(4바이트) */
#define DSIZE       8             /* double word(8바이트 정렬 단위) */
#define CHUNKSIZE   (1 << 12)     /* 힙 확장 시 기본 요청 크기(4096바이트) */

/* 유틸 매크로 */
#define MAX(x, y)       ((x) > (y) ? (x) : (y))          /* 최대값 */
#define PACK(size, a)   ((size) | (a))                   /* 헤더/풋터에 (크기|할당비트) 패킹 */

/* 메모리 접근 매크로 (p는 void* 또는 char* 포인터여야 함) */
#define GET(p)          (*(unsigned int *)(p))           /* p가 가리키는 곳의 4바이트 값 읽기 */
#define PUT(p, val)     (*(unsigned int *)(p) = (val))   /* p가 가리키는 곳에 4바이트 값 쓰기 */

/* 헤더/풋터로부터 정보 추출 */
#define GET_SIZE(p)     (GET(p) & ~0x7)                  /* 하위 3비트를 제외한 블록 크기 */
#define GET_ALLOC(p)    (GET(p) & 0x1)                   /* 할당 여부(하위 비트) */

/* 블록 포인터(bp)로부터 헤더/풋터의 주소 얻기 */
#define HDRP(bp)        ((char *)(bp) - WSIZE)           /* 현재 블록의 헤더 주소 */
#define FTRP(bp)        ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) /* 현재 블록의 풋터 주소 */

/* 인접 블록으로 이동 */
#define NEXT_BLKP(bp)   ((char *)(bp) + GET_SIZE(HDRP(bp)))         /* 다음 블록의 bp */
#define PREV_BLKP(bp)   ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE)) /* 이전 블록의 bp (이전 풋터 이용) */

/* 정렬 관련(참고용: mm_malloc에서 직접 사용하지는 않음) */
#define ALIGNMENT 8
#define ALIGN(size)     (((size) + (ALIGNMENT - 1)) & ~0x7)         /* 8바이트 배수로 반올림 */
#define SIZE_T_SIZE     (ALIGN(sizeof(size_t)))                     /* size_t 저장 시 정렬된 크기 */

/* ====== 전역 변수 ====== */
static char *heap_listp = NULL;   /* 힙의 prologue 블록 바로 뒤를 가리키는 포인터(일반적으로 첫 bp 기준점) */
static char *rover = NULL;     // ← next-fit 탐색 시작 지점

/* ====== 내부 함수 프로토타입 ====== */
static void *extend_heap(size_t words);   /* 힙을 words(워드) 만큼 확장 */
static void *coalesce(void *bp);          /* 인접 free 블록 병합 */
static void *find_fit(size_t asize);      /* first-fit or next-fit 탐색 */
static void place(void *bp, size_t asize);/* 블록 배치 및 필요 시 분할 */

/* ------------------------------------------------------ */
/* mm_init - 힙 초기화: prologue/epilogue 생성 후 초기 확장 */
/* ------------------------------------------------------ */
int mm_init(void)
{
    /* prologue(프롤로그) + epilogue(에필로그)용으로 4워드 공간 요청 */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1) return -1;

    /* 프롤로그 구성
       [패딩][프롤로그 헤더][프롤로그 풋터][에필로그 헤더] */
    PUT(heap_listp + 0*WSIZE, 0);                 /* Alignment padding (사용 안 함) */
    PUT(heap_listp + 1*WSIZE, PACK(DSIZE, 1));    /* Prologue header: 크기=8, 할당=1 */
    PUT(heap_listp + 2*WSIZE, PACK(DSIZE, 1));    /* Prologue footer: 크기=8, 할당=1 */
    PUT(heap_listp + 3*WSIZE, PACK(0, 1));        /* Epilogue header: 크기=0, 할당=1 */
    heap_listp += (2 * WSIZE);                    /* heap_listp를 프롤로그의 payload 위치로 이동 */

    /* 초기 힙 확장: CHUNKSIZE 바이트 만큼 가용 블록 생성 */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL) return -1;
    rover = NEXT_BLKP(heap_listp);   // 프롤로그 다음 블록부터 시작
    return 0;
}

/* ------------------------------------------------------ */
/* extend_heap - 힙을 words(워드) 만큼 확장하여 새 free 블록 생성 */
/*               짝수 워드로 맞춰 8바이트 정렬 유지                */
/* ------------------------------------------------------ */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* 짝수 워드로 반올림(8바이트 정렬 유지) */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* 힙 확장(mem_sbrk) */
    if ((long)(bp = mem_sbrk(size)) == -1) return NULL;

    /* 새로 얻은 영역을 하나의 큰 free 블록으로 초기화 */
    PUT(HDRP(bp), PACK(size, 0));                  /* Free block header */
    PUT(FTRP(bp), PACK(size, 0));                  /* Free block footer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));          /* New epilogue header (끝 표시) */

    /* 이전 블록이 free였다면 병합해서 단편화 감소 */
    rover = bp;                           // ← 새로 확장된 free 블록으로 이동

    return coalesce(bp);
}

/* ------------------------------------------------------ */
/* coalesce - 인접한 free 블록을 즉시 병합하여 큰 블록 확보 */
/*   prev_alloc / next_alloc 조합에 따라 4가지 경우 처리     */
/* ------------------------------------------------------ */
static void *coalesce(void *bp)
{
    /* 이전 블록의 할당 여부: 이전 블록의 풋터를 보고 판단 */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    /* 다음 블록의 할당 여부: 다음 블록의 헤더를 보고 판단 */
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    /* 현재 블록의 크기 */
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) { /* Case 1: 양쪽 모두 할당 -> 병합 없음 */
        return bp;
    }
    else if (prev_alloc && !next_alloc) { /* Case 2: 다음만 free -> 현재와 다음 병합 */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));     /* 다음 블록 크기 더하기 */
        PUT(HDRP(bp), PACK(size, 0));              /* 새 크기로 현재 헤더 갱신 */
        PUT(FTRP(bp), PACK(size, 0));              /* 새 크기로 현재 풋터 갱신 */
    }
    else if (!prev_alloc && next_alloc) { /* Case 3: 이전만 free -> 이전과 현재 병합 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));     /* 이전 블록 크기 더하기 */
        PUT(FTRP(bp), PACK(size, 0));              /* 새 크기로 최종 풋터 갱신 */
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));   /* 이전 블록 헤더를 새 크기로 */
        bp = PREV_BLKP(bp);                         /* bp를 병합된 블록의 시작으로 이동 */
    }
    else { /* Case 4: 양쪽 모두 free -> 세 블록(이전,현재,다음) 전부 병합 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)))      /* 이전 블록 크기 */
              + GET_SIZE(FTRP(NEXT_BLKP(bp)));     /* 다음 블록 크기
                                                      (참고: HDRP(NEXT_BLKP(bp))를 써도 동일) */
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));   /* 병합된 헤더(이전 블록 헤더 위치) */
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));   /* 병합된 풋터(다음 블록 풋터 위치) */
        bp = PREV_BLKP(bp);                         /* bp를 병합된 블록의 시작으로 이동 */
    }
    rover = bp;   // ← 병합 결과 블록으로 로버 보정(충분히 안전하고 단순)
    return bp;
}

/* ------------------------------------------------------ */
/* find_fit - first-fit 방식으로 asize 이상 수용 가능한 가용 블록 탐색 */
/*   순회 시작: 프롤로그 다음 블록부터, 에필로그(크기 0) 직전까지        */
/* ------------------------------------------------------ */
static void *find_fit(size_t asize)
{
    void *bp;

    /*First Fit*/
    /* heap_listp는 프롤로그 payload를 가리키므로 그 다음 블록부터 검사 */

    // for (bp = NEXT_BLKP(heap_listp);               /* 첫 실제 블록 */
    //      GET_SIZE(HDRP(bp)) > 0;                   /* 에필로그(사이즈 0) 전까지 */
    //      bp = NEXT_BLKP(bp))                       /* 다음 블록으로 이동 */
    // {
    //     if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp)))) {
    //         return bp;                             /* 처음 맞는 free 블록 반환 */
    //     }
    // }

    /*Next Fit*/
       /* 1) rover에서 에필로그 직전까지 */
    for (bp = rover; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp))))
            return bp;
    }

    /* 2) 못 찾으면 리스트의 앞(프롤로그 다음)부터 rover 직전까지 */
    for (bp = NEXT_BLKP(heap_listp); bp != rover; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp))))
            return bp;
    }


    return NULL;                                   /* 적합 블록 없음 */
}

/* ------------------------------------------------------ */
/* place - 찾은 free 블록 bp에 asize만큼 배치, 남으면 분할      */
/*   분할 임계: 남는 공간이 최소 블록(16B) 이상일 때만 분할        */
/* ------------------------------------------------------ */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));             /* 현재 free 블록의 총 크기 */

    if ((csize - asize) >= (2 * DSIZE)) {          /* 분할 가능한 충분한 여유(>=16) */
        PUT(HDRP(bp), PACK(asize, 1));             /* 앞쪽 조각을 할당 상태로 설정 */
        PUT(FTRP(bp), PACK(asize, 1));

        bp = NEXT_BLKP(bp);                        /* 남은 뒷부분을 새 free 블록으로 설정 */
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
         rover = bp; // ← 남은 free 조각부터 다음 탐색 시작
    } else {                                       /* 분할하지 않고 전부 할당 */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
            rover = NEXT_BLKP(bp); // ← 바로 다음 블록부터 시작
    }
}

/* ------------------------------------------------------ */
/* mm_malloc - 크기 size의 블록 요청 처리                     */
/*   1) 요청 정규화(asize) → 2) 가용 블록 탐색 → 3) 없으면 확장   */
/* ------------------------------------------------------ */
void *mm_malloc(size_t size)
{
    if (size == 0) return NULL;                    /* 0바이트 요청은 NULL 반환(관례) */

    size_t asize;                                  /* 헤더/풋터 포함·정렬된 크기 */

    /* 최소 블록 보장: 요청이 8바이트 이하라면 블록 전체 16바이트로 */
    if (size <= DSIZE)
        asize = 2 * DSIZE;                         /* 16바이트 */
    else
        /* size + 헤더/풋터(=8) 후 8바이트 배수로 반올림 */
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    /* 1) first-fit 탐색 */
    void *bp = find_fit(asize);
    if (bp != NULL) {
        place(bp, asize);
        return bp;                                 /* 배치한 payload 포인터 반환 */
    }

    /* 2) 적합 블록이 없으면 힙 확장 후 배치 */
    size_t extendsize = MAX(asize, CHUNKSIZE);     /* 한번에 최소 CHUNKSIZE만큼 늘림 */
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;                               /* 확장 실패 시 NULL */
    place(bp, asize);
    return bp;
}

/* ------------------------------------------------------ */
/* mm_free - 블록 해제 후 인접 free 블록과 즉시 병합            */
/* ------------------------------------------------------ */
void mm_free(void *bp)
{
    if (bp == NULL) return;                        /* NULL free 방어 */

    size_t size = GET_SIZE(HDRP(bp));              /* 현재 블록의 전체 크기 */
    PUT(HDRP(bp), PACK(size, 0));                  /* 헤더를 free로 마킹 */
    PUT(FTRP(bp), PACK(size, 0));                  /* 풋터를 free로 마킹 */
    coalesce(bp);                                  /* 인접 free 블록과 병합 */
}

/* ------------------------------------------------------ */
/* mm_realloc - 단순 구현: 새로 할당 → 최소크기만큼 복사 → 원래 free */
/*   최적화 아이디어(참고):
/*     - 오른쪽 블록이 free면 흡수하여 in-place 확장 시도 */
/*     - 줄이는 경우엔 분할하여 뒤쪽을 free로 돌려주기     */
/* ------------------------------------------------------ */
void *mm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) return mm_malloc(size);       /* realloc(NULL, s) == malloc(s) */
    if (size == 0)   { mm_free(ptr); return NULL;} /* realloc(p, 0) == free(p) */

    /* 새 블록 요청 */
    void *newptr = mm_malloc(size);
    if (newptr == NULL) return NULL;

    /* 복사 크기: 기존 payload 크기와 새 요청 중 작은 값 */
    size_t oldsize = GET_SIZE(HDRP(ptr)) - DSIZE;  /* 기존 블록의 payload 크기(헤더/풋터 제외) */
    size_t copySize = (size < oldsize) ? size : oldsize;
    memcpy(newptr, ptr, copySize);

    /* 기존 블록 해제 */
    mm_free(ptr);
    return newptr;
}
