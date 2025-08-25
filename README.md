# 📘 Docker + VSCode DevContainer 기반 C 개발 환경

## C언어 사용 Malloc 구현

### int mm_init(void)                          - 힙 초기화: prologue/epilogue 생성 후 초기 확장 
### void *mm_malloc(size_t size)               - 크기 size의 블록 요청 처리
### void mm_free(void *bp)                     - 블록 해제 후 인접 free 블록과 즉시 병합
### void *mm_realloc(void *ptr, size_t size)   - 새로 할당 → 최소크기만큼 복사 → 원래 free 

### static void *extend_heap(size_t words);    - 힙을 words(워드) 만큼 확장 
### static void *coalesce(void *bp);           - 인접 free 블록 병합 
### static void *find_fit(size_t asize);       - first-fit or next-fit 탐색 
### static void place(void *bp, size_t asize); - 블록 배치 및 필요 시 분할 
