# 10: API 서버 첫 왕복

**What to build:** 외부 클라이언트가 `curl --data-binary @q.sql http://127.0.0.1:<포트>/query`로 SQL 문장 하나를 보내면, 서버가 스레드 풀에서 엔진을 실행하고 결과를 JSON으로 돌려줍니다. SQL 오류와 저장 오류는 상태 코드와 기계가 읽는 오류 이유로 구분됩니다. 결정은 `docs/prd/final.md` Phase 3 절을 따르고, 상태 코드 표는 DA 초안 7.3절을 옮긴 아래 표를 따릅니다(DA 초안은 이 브랜치에 없습니다).

**Blocked by:** 07, 09

**Status:** ready-for-agent

작업에 포함되는 것:

- **Mongoose**: Mongoose 7.23(`mongoose.c`, `mongoose.h`)을 저장소에 넣습니다. mini-redis처럼 출처와 버전을 빌드 설정 주석에 적고, 저장소 루트에 GPLv2 전문을 담은 LICENSE를 둡니다. Mongoose 전용 컴파일 옵션은 `mongoose.c`에만 줍니다. 엔진과 그 테스트에는 Mongoose를 링크하지 않습니다.
- **스레드 풀**: 작업 스레드와 작업 대기열을 직접 만듭니다(L29). 기본은 스레드 8개, 대기열 128칸이고 실행 인자로 바꿉니다. 이벤트 루프는 헤더와 본문을 받은 요청의 SQL만 복사해 대기열에 넣습니다. 작업 스레드는 엔진을 API 모드(다음 문장 위치 인자 없이)로 부르고, JSON을 만들어 완료 대기열에 넣은 뒤 `mg_wakeup()`으로 이벤트 루프를 깨우기만 합니다. 결과나 포인터를 `mg_wakeup()`에 싣지 않습니다. 응답은 이벤트 루프만 보내고, 매 poll에서 완료 대기열을 확인합니다. 연결이 끊긴 요청의 결과는 해제합니다.
- **요청**: `POST /query`, 본문은 SQL 문장 하나입니다. API 모드에서는 문장 뒤에 다른 문장이 있으면 실행 전에 거절합니다.
- **응답**: SELECT 성공은 `200`과 `{"fields": [...], "records": [[...]], "access_path": "INDEX", "records_examined": 1}`, INSERT 성공은 `200`과 `{"inserted": 1, "id": 3}`, 오류는 `{"error": "<이유>", "message": "<사람이 읽는 설명>"}`입니다. `INT`와 `id`는 double로 바꾸지 않고 JSON 숫자로 씁니다.
- **주소**: 기본은 127.0.0.1에 묶습니다. 포트와 주소는 실행 인자로 바꿉니다. 주소를 바꾸는 옵션은 시연용이며, 인증이 없다는 점을 사용법에 적습니다.
- **기능 테스트 틀**: Python 기능 테스트가 서버를 띄우고 실제 HTTP 요청을 보냅니다(L49).
- **문서**: `README.md`에 API 절(요청, 응답, 상태 코드)을 쓰고, "알려진 한계"에 "JSON 정수는 JavaScript 안전 정수 범위 ±(2^53 − 1)를 넘으면 클라이언트에서 정밀도를 잃는다"를 더합니다. `docs/requirements.md`의 L27\~L30, L43, L44 행을 채웁니다.

## 이 티켓이 맡는 상태 코드 (DA 초안 7.3절에서 옮김)

| 상태 | 오류 이유 | 언제 |
|---|---|---|
| `400` | `syntax_error`, `table_not_found`, `field_not_found`, `type_mismatch`, `value_count_mismatch`, `multiple_statements` | SQL 오류, 문장이 둘 이상 |
| `404` | `route_not_found` | `/query`가 아닌 경로 |
| `405` | `method_not_allowed` | `/query`에 POST가 아닌 메서드. `Allow: POST` 헤더를 같이 보냄 |
| `500` | `storage_error`, `table_unavailable`, `out_of_memory` | 티켓 05의 실패 표 |

본문 길이, 빈 본문, chunked, 본문 상한, 결과 상한, 대기열 가득 참은 티켓 11이 맡습니다.

완료 조건:

- [ ] `curl`로 INSERT를 보내면 `200`과 발급된 `id`를 받고, 같은 `id`의 SELECT가 `access_path: "INDEX"`로 그 record를 돌려줍니다.
- [ ] 다른 field 조건의 SELECT는 `access_path: "SCAN"`과 전체 record 수를 돌려줍니다.
- [ ] 결과 0건인 SELECT는 `200`과 빈 `records`입니다.
- [ ] 위 표의 오류 이유마다 상태 코드와 JSON이 맞습니다.
- [ ] 문장 두 개가 든 본문은 `400 multiple_statements`이고 첫 문장도 실행되지 않습니다.
- [ ] 클라이언트 여러 개가 동시에 INSERT와 SELECT를 보내도 티켓 09의 동시성 조건이 서버를 거쳐서도 지켜집니다.
- [ ] 기본 실행에서 127.0.0.1이 아닌 주소로는 접속되지 않습니다.
- [ ] 엔진 단위 테스트는 Mongoose 없이 빌드됩니다.
