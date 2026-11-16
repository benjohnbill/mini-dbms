<!-- readme: archetype=team -->
# mini-dbms

Krafton Jungle Week 11-12: SQL processor, B+ tree index, and a thread-pool API server

[설치 및 실행](#설치-및-실행) · [API](#api) · [구조](#구조) · [Phase](#phase) · [성능 비교](#성능-비교) · [알려진 한계](#알려진-한계) · [파일 트리](#파일-트리) · [문서](#문서)

> [!NOTE]
> 이 README의 내용은 자리만 잡아 둔 것입니다. 팀 회의에서 최종 PRD가 정해지면 각 절을 최종 PRD의 내용으로 바꿉니다. 각자의 PRD 초안은 [`docs/prd/`](docs/prd/)에 있습니다.

## 설치 및 실행

언어와 도구는 최종 PRD에서 정합니다. 검증 명령(`make verify` 또는 그 언어의 같은 역할 명령)과 CI도 그때 만듭니다.

```bash
# 최종 PRD 확정 후 작성: 빌드, 테스트, CLI 실행, 서버 실행
```

## API

Phase 3에서 외부 클라이언트가 DBMS를 부르는 방법입니다. 프로토콜(HTTP 등)과 요청, 응답, 오류의 형식은 최종 PRD에서 정합니다.

## 구조

SQL 한 문장이 처리기, 인덱스, 테이블 파일을 지나는 흐름과, API 서버와 스레드 풀이 그 엔진을 감싸는 모양을 그림 한 장으로 보여 줍니다. 최종 PRD 확정 후 그립니다.

## Phase

LMS는 과제를 세 단계로 나눕니다. 마일스톤은 따로 두지 않고, 최종 PRD 하나가 세 Phase를 모두 다룹니다.

| Phase | 주제 | 요구사항 | 상태 |
|---|---|---|---|
| 1 | SQL 처리기: CLI, SQL 파싱과 실행, 파일 기반 저장 | L7\~L19, L35\~L38, L47 | 진행 전 |
| 2 | B+ 트리 인덱스: ID 자동 부여, 100만 건 삽입, 인덱스 사용과 선형 탐색의 속도 비교 | L20\~L26, L39\~L41, L48 | 진행 전 |
| 3 | API 서버: 스레드 풀로 SQL 요청을 병렬 처리 | L27\~L30, L42\~L44, L49 | 진행 전 |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L50(엣지 케이스)은 모든 Phase의 테스트에 들어갑니다.

## 성능 비교

Phase 2의 ID 기준 SELECT(인덱스 사용)와 다른 필드 기준 SELECT(선형 탐색)의 속도 차이를 적습니다. 측정 기록은 [`docs/measurements/`](docs/measurements/)에 측정 한 번마다 파일 하나로 남깁니다.

## 알려진 한계

최종 PRD의 Out of Scope에서 옮깁니다.

## 파일 트리

```
mini-dbms/
├─ docs/
│  ├─ prd/              각자의 PRD 초안(<이니셜>.md)과 최종 PRD
│  ├─ requirements.md   LMS 요구사항과 우리 해석
│  ├─ measurements/     측정 기록
│  ├─ adr/              큰 설계 결정
│  └─ agents/           AI 스킬 설정
└─ (src/, tests/, 빌드 파일은 최종 PRD 확정 후)
```

## 문서

- [`GLOSSARY.md`](GLOSSARY.md): 코드와 문서에서 쓰는 용어
- [`CONVENTION.md`](CONVENTION.md): 팀 규칙, 작업 흐름, 문서 역할, 측정 기록
- [`docs/prd/`](docs/prd/): PRD 초안과 최종 PRD
- [`docs/requirements.md`](docs/requirements.md): 요구사항 표
- [`docs/measurements/`](docs/measurements/): 측정 기록. 측정 한 번마다 파일 하나
- [`docs/adr/`](docs/adr/): 큰 설계 결정
- [`AGENTS.md`](AGENTS.md): AI 에이전트가 먼저 읽는 파일

### Team

이민호 · 안다빈 · 조진근
