# 05: 실패한 INSERT를 되돌리기

**What to build:** INSERT가 파일 쓰기에 실패하면 테이블 파일이 쓰기 전과 같아지고, 실패한 INSERT는 ID를 쓰지 않습니다. 되돌리기도 실패하면 그 테이블만 멈추고, 다른 테이블은 계속 쓸 수 있습니다. 회의(2026-11-16)에서 이 순서를 정했고, 되돌리는 방법과 오류는 AI에게 맡겼습니다. B+ 트리와 관련된 단계는 티켓 07이, HTTP 상태 코드는 티켓 10이 맡습니다.

**Blocked by:** 03

**Status:** ready-for-agent

## 테이블마다 메모리에 두는 상태

- `committed_size`: 마지막 완전한 record의 끝 위치입니다(티켓 01, 03). `fstat` 값이 아닙니다.
- `next_id`
- `available`: 시작할 때 참이고, 되돌리기가 실패하면 거짓이 됩니다.

## INSERT 순서

1. 값을 검증하고 record를 버퍼에 만듭니다. 메모리 할당은 모두 파일을 건드리기 전에 끝냅니다.
2. 버퍼를 `committed_size` 위치에 `pwrite`로 씁니다. 일부만 쓰이면 나머지를 이어 쓰고, `EINTR`이면 다시 시도합니다.
3. `committed_size`에 record 길이를 더하고 `next_id`를 1 올린 뒤 성공을 돌려줍니다.

SCAN은 `committed_size`까지만 읽습니다. 그래서 반쯤 쓰인 record를 보지 않습니다.

## 실패했을 때

| 실패 | 엔진이 하는 일 | 파일 | ID | 오류 이유 |
|---|---|---|---|---|
| 1단계 메모리 할당 실패 | 오류 반환 | 그대로 | 쓰지 않음 | `out_of_memory` |
| 2단계 쓰기 실패(`ENOSPC`, `EIO`, `EFBIG`) 후 `ftruncate(committed_size)` 성공 | 오류 반환 | 쓰기 전과 같음 | 쓰지 않음 | `storage_error` |
| 2단계 쓰기 실패 후 `ftruncate`도 실패 | `available = false`, 오류 반환 | 알 수 없음 | 쓰지 않음 | `table_unavailable` |
| `available = false`인 테이블에 온 요청 | 읽거나 쓰지 않고 오류 반환 | 그대로 | — | `table_unavailable` |

되돌리기가 실패한 `table_unavailable` 메시지에는 "이 INSERT가 저장됐는지 알 수 없다"고 적습니다. 재시작하면 `available`이 풀리고, 시작할 때 읽기(티켓 04)가 파일을 판정합니다. CLI는 오류를 stderr에 쓰고 종료 코드 1로 끝납니다.

## 테스트를 위한 틈

테이블 코드는 파일 쓰기와 자르기를 함수 포인터 표로 부르고, 테스트가 그 포인터를 바꿉니다. 실제 코드는 `pwrite`와 `ftruncate`를 씁니다. 이 틈이 이번 스펙에서 새로 만드는 유일한 틈입니다. 첫 번째 실패는 실제로도 만들 수 있습니다. `RLIMIT_FSIZE`를 지금 파일 크기로 두면 다음 쓰기가 `EFBIG`로 실패합니다. 그 테스트에서는 `SIGXFSZ`를 무시합니다.

완료 조건:

- [ ] 쓰기가 실패하고 되돌리기가 성공하면 파일 크기와 내용이 그대로이고, 다음 INSERT는 실패한 INSERT가 쓰지 못한 ID를 받습니다.
- [ ] 쓰기가 실패하고 되돌리기가 성공한 뒤의 SELECT는 실패 전과 같은 record를 돌려줍니다.
- [ ] 되돌리기가 실패하면 그 INSERT와 이후 같은 테이블의 SELECT, INSERT가 모두 `table_unavailable`입니다.
- [ ] 한 테이블이 멈춰도 다른 테이블의 요청은 성공합니다.
- [ ] 일부만 쓰인 뒤 나머지가 이어 쓰이면 완전한 record 하나가 됩니다.
- [ ] 메모리 할당 실패는 파일을 바꾸지 않습니다.
