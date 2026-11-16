# 07: `WHERE id = k`가 B+ 트리로 record 하나만 읽기

**What to build:** `SELECT … WHERE id = 정수`를 실행하면 엔진이 B+ 트리에서 위치를 찾아 그 record 하나만 읽고, 결과에 접근 경로 `INDEX`와 읽은 record 수 1이 붙습니다. 다른 field 조건은 그대로 SCAN입니다. 재시작하면 테이블 파일에서 트리를 다시 만듭니다. 이로써 엔진이 "어떻게 찾을지"를 스스로 정합니다. 결정은 `docs/prd/final.md` Phase 2 절의 재구성, planner, 결과 메타를 따릅니다.

**Blocked by:** 02, 05, 06

**Status:** ready-for-agent

작업에 포함되는 것:

- **재구성**: 티켓 03의 시작할 때 읽기가 record마다 `id → record 시작 위치`를 테이블의 트리에 넣습니다. 인덱스 파일은 없습니다.
- **INSERT 순서에 트리 끼우기**: 티켓 05의 순서에서 1단계(파일을 건드리기 전)에 노드를 미리 할당하고, 파일 쓰기가 성공한 뒤 `committed_size`를 올리기 전에 트리에 `id → 쓰기 전 committed_size`를 넣습니다. 미리 할당이 실패하면 `out_of_memory`이고 파일은 바뀌지 않습니다.
- **planner**: 검증을 통과한 SELECT에서, 조건이 `id = 정수`이면 INDEX, 그 밖은 SCAN입니다. INDEX는 트리에서 위치를 찾고 그 record 하나만 `pread`로 읽는 iterator입니다. 실행기는 SCAN과 INDEX를 같은 iterator 모양으로 받습니다. INDEX로 찾은 record를 조건과 다시 비교하지는 않습니다.
- **결과 메타**: 결과 구조체에 접근 경로와 읽은 record 수를 담습니다. 티켓 02의 stderr 메타 줄이 `INDEX` 값을 보입니다. 없는 ID는 `INDEX`, 결과 0건, 읽은 record 수 0입니다.
- **문서**: `docs/requirements.md`의 L20\~L22, L39, L48(정확성) 행을 채웁니다.

완료 조건 (L48):

- [ ] `WHERE id = 7`(있는 ID)은 접근 경로 `INDEX`, 결과 1건, 읽은 record 수 1입니다.
- [ ] ID가 1\~N인 테이블에서 `WHERE id = N+1`은 `INDEX`, 0건, 읽은 record 수 0입니다.
- [ ] `WHERE email = '…'`은 `SCAN`이고 읽은 record 수는 전체 record 수입니다.
- [ ] `WHERE id = 'abc'`는 타입 오류이고 경로를 고르지 않습니다.
- [ ] INSERT 직후의 `WHERE id = 새 ID`가 INDEX로 그 record를 찾습니다.
- [ ] CLI를 다시 실행한 뒤에도 `WHERE id = k`가 INDEX로 같은 record를 찾습니다.
- [ ] 트리 미리 할당이 실패하면 INSERT가 `out_of_memory`이고 파일과 트리가 바뀌지 않습니다.
- [ ] 같은 조건의 INDEX 결과와 SCAN 결과가 같습니다(측정 전용 강제 SCAN 경로는 티켓 08).
