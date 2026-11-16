#!/usr/bin/env python3
"""측정용 users 테이블의 INSERT 문을 SQL 파일로 만듭니다.

사용법: python3 scripts/generate-users.py <record 수> <시드> <출력 파일>

스키마는 users(name TEXT, email TEXT, age INT)입니다. n번째 INSERT는
name 'user0000n', email 'user0000n@example.com'을 담으므로, 빈 테이블에 넣으면
ID n과 그 번호가 같고 email 값이 겹치지 않습니다. 그래서 `WHERE id = n`과
`WHERE email = 'user0000n@example.com'`이 같은 record 1건을 돌려줍니다.
시드는 age(18부터 80까지)만 정하고, 시드가 같으면 바이트까지 같은 파일이 나옵니다.
"""

import random
import sys

AGE_MIN = 18
AGE_MAX = 80


def main(argv):
    if len(argv) != 4 or not argv[1].isdigit() or not argv[2].isdigit():
        print("usage: generate-users.py <records> <seed> <output.sql>",
              file=sys.stderr)
        return 2
    records, seed, out_path = int(argv[1]), int(argv[2]), argv[3]
    ages = random.Random(seed)
    # newline="\n": Windows에서 실행해도 같은 바이트가 나오게 합니다.
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        for number in range(1, records + 1):
            name = f"user{number:07d}"
            age = ages.randint(AGE_MIN, AGE_MAX)
            out.write(f"INSERT INTO users VALUES ('{name}', "
                      f"'{name}@example.com', {age});\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
