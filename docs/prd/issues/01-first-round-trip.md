# 01: CLI로 INSERT 한 행과 SELECT *가 도는 첫 왕복

**What to build:** 팀원이 스키마 파일과 빈 테이블 파일을 미리 만든 데이터 폴더에서 CLI에 SQL 파일을 넘기면, `INSERT INTO users VALUES ('kim', 'kim@example.com', 20);`이 record 하나를 바이너리 테이블 파일에 덧붙이고 `SELECT * FROM users;`가 그 record를 stdout에 TSV로 출력합니다. 빌드, `make verify`, CI(macOS와 Linux), acutest 단위 테스트, `tests/functional/` 기능 테스트 틀은 `main`에 이미 있습니다(PR #3). 이 티켓은 그 틀 위에서 `src/cli.c`의 인자 검사만 있는 진입점부터 시작해 첫 엔진 코드와 첫 테스트를 더하고, 뒤 티켓은 모두 이 엔진 위에서 동작을 늘립니다. 결정은 `docs/prd/final.md`의 공통과 Phase 1 절을 따릅니다.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

작업에 포함되는 것:

- **시작점**: `src/cli.c`(인자 검사만 있는 `main`)와 `Makefile`입니다. 엔진 코드는 `src/`의 다른 `.c` 파일에 두면 `Makefile`이 CLI와 단위 테스트에 함께 링크합니다. 처음 만드는 기능 테스트가 `tests/functional/`의 첫 파일이 되어 `make functional`이 실제로 무언가를 돌리게 됩니다. glibc 전용 기능은 쓰지 않습니다. `tests/harness_test.c`는 엔진 테스트가 생기면 지웁니다.
- **엔진 함수 네 개**: 엔진 만들기(데이터 폴더, 옵션), 문장 하나 실행, 결과 해제, 엔진 해제입니다. 실행 함수는 문장 하나만 실행하고, 세 번째 인자로 다음 문장의 시작 위치를 돌려줍니다(CLI 모드). 결과 구조체는 성공, 문장 없음, 오류 중 하나의 상태를 담고, 성공한 SELECT는 field 이름과 record를, 성공한 INSERT는 발급한 ID를 담습니다. 엔진은 결과를 구조체에 모두 모아 돌려줍니다.
- **SQL**: 토크나이저와 재귀 하강 파서가 `INSERT INTO 테이블 VALUES (정수 또는 '문자열', …);`과 `SELECT * FROM 테이블;`만 이해합니다. 나머지 문법은 티켓 02가 더합니다.
- **스키마 파일**: 테이블마다 스키마 파일 하나를 데이터 폴더에 둡니다. 한 줄에 `name TEXT`처럼 field 하나를 적고, `id`는 적지 않습니다. 테이블 이름은 파일 이름입니다. 이 티켓은 테이블 하나만 다룹니다.
- **테이블 파일**: 미리 만든 빈 파일을 씁니다. 엔진은 테이블 파일을 만들지 않습니다(없는 파일의 오류는 티켓 03). record는 본문 길이(u32) + 본문이고, 본문은 `id`(i64)와 스키마 순서의 field 값입니다. `INT`는 i64, `TEXT`는 바이트 길이(u32) + UTF-8 바이트입니다. 모든 정수는 리틀 엔디언입니다. 파일은 `O_APPEND` 없이 열고, 엔진이 메모리에 들고 있는 파일 끝 위치(`committed_size`)에 `pwrite`로 씁니다. Linux에서는 `O_APPEND`로 연 파일에 `pwrite`하면 위치 인자를 무시하기 때문입니다.
- **SCAN**: 엔진 안의 record 읽기는 iterator(다음 record, 닫기) 모양으로 만듭니다. 티켓 07이 같은 모양으로 INDEX 경로를 더합니다.
- **자동 ID**: 1부터 1씩 올립니다. 이 티켓은 빈 데이터 폴더에서 한 번 실행하는 경우만 다룹니다. 재실행할 때 ID를 이어 가는 일은 티켓 03이 합니다.
- **CLI**: `[--data 폴더] <SQL 파일>`입니다. `--data`를 생략하면 `data/`를 씁니다. SQL 파일 전체를 메모리에 읽고, 다음 문장 위치를 받아 문장 없음이 나올 때까지 반복합니다. SELECT 결과는 stdout에 TSV(첫 줄은 field 이름, 첫 field는 `id`)로 씁니다. `TEXT` 안의 탭, 줄바꿈, 역슬래시는 `\t`, `\n`, `\\`로 적습니다. 결과가 0건이면 field 이름 줄만 씁니다. 종료 코드는 0(성공), 1(파일·SQL·실행 오류), 2(잘못된 CLI 인자)입니다.
- **문서**: `docs/requirements.md`의 L8, L10~L19, L36, L38 행에 `해석`, `충족`, `근거`를 채웁니다. `README.md`의 "알려진 한계"에 "`fsync`를 하지 않아 정전 때 성공을 알린 record를 잃을 수 있다"를 적습니다. `README.md`의 설치와 실행 절에 빌드와 CLI 실행 방법을 적습니다.

완료 조건:

- [ ] `make verify`가 macOS와 Linux CI에서 통과하고, `make functional`이 이 티켓의 기능 테스트를 실행합니다.
- [ ] 빈 데이터 폴더에서 INSERT 두 개와 `SELECT *`를 담은 SQL 파일을 실행하면 ID 1, 2인 record 두 개가 TSV로 나옵니다.
- [ ] 같은 실행 뒤 테이블 파일의 바이트가 위 레이아웃과 같습니다. 기능 테스트가 기대 바이트를 직접 적어 비교합니다.
- [ ] 탭, 줄바꿈, 역슬래시가 든 `TEXT`가 이스케이프되어 출력됩니다.
- [ ] 빈 테이블의 `SELECT *`는 field 이름 줄만 출력합니다.
- [ ] 잘못된 CLI 인자는 종료 코드 2, 열 수 없는 SQL 파일은 종료 코드 1입니다.
- [ ] 엔진 단위 테스트가 엔진 함수 네 개만 불러 INSERT와 SELECT 결과를 확인합니다.
