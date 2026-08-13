# C언어로 로켓 날리기

아리안 5 Flight 501 사고에서 확인된 형변환 실패와 오류 전파를 C언어로 단순화한 교육용 로켓 비행 제어 시뮬레이터입니다.

동일한 입력을 두 모드로 실행합니다.

- `UNSAFE`: 두 관성 기준 장치(SRI)가 같은 형변환 오류로 차례로 정지하고, 진단 데이터가 비행 데이터로 해석되어 제어 상실과 기체 파괴로 이어집니다.
- `SAFE`: 이륙 후 불필요한 정렬 작업을 중단하고 형변환과 입력 데이터도 검증하여 정상 비행을 계속합니다.

이 프로젝트는 실제 로켓의 물리 모델이나 아리안 5 소프트웨어를 복제하지 않습니다. 대신 조사보고서에 기록된 핵심 사건의 순서와 작은 자료형 오류가 센서, 항법, 제어 단계로 전파되는 과정을 비교합니다.

## 실행 화면

C 프로그램은 T+0부터 T+60까지 콘솔 로그를 출력하고, 다음 CSV를 생성합니다.

```text
output/unsafe.csv
output/safe.csv
```

웹 화면은 두 CSV를 읽어 UNSAFE와 SAFE 궤적을 좌우 SVG에서 같은 시간축으로 부드럽게 재생합니다. UNSAFE는 T+37에 `CONTROL_LOST`가 되고 T+39에 폭발과 잔해 효과가 나타납니다. 재생, 일시정지, 처음으로 이동, 시간 슬라이더를 지원합니다.

## 빠른 실행

필요한 프로그램은 다음과 같습니다.

- Windows PowerShell
- MinGW GCC
- Python 3

프로젝트 폴더에서 실행합니다.

```powershell
.\run.ps1
```

스크립트는 다음 작업을 자동으로 수행합니다.

1. GCC C11 컴파일
2. 16비트 형변환 경계값 테스트
3. UNSAFE와 SAFE 시뮬레이션 및 CSV 생성
4. CSV 시나리오 검증
5. `http://localhost:8000/web/` 실행

PowerShell 실행 정책으로 스크립트가 차단되면 현재 실행에만 정책을 우회할 수 있습니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1
```

웹 서버를 열지 않고 컴파일과 자동 검증만 실행하려면 다음 옵션을 사용합니다.

```powershell
.\run.ps1 -SkipServer
```

## C 코드의 핵심

실제 SRI의 Ada 코드는 보호되지 않은 변환에서 `Operand Error`를 발생시켰습니다. C에서 범위를 벗어난 실수를 작은 정수형으로 직접 변환하면 정의되지 않은 동작이 되므로, 이 프로그램은 오류 결과를 명시적으로 모델링합니다.

```c
if (value < (double)INT16_MIN || value > (double)INT16_MAX) {
    return mode == MODE_UNSAFE
               ? CONVERSION_OPERAND_ERROR
               : CONVERSION_BLOCKED;
}

*result = (int16_t)value;
```

따라서 실제 정의되지 않은 동작에 의존하지 않으면서도 UNSAFE의 프로세서 정지와 SAFE의 방어 동작을 항상 같은 방식으로 확인할 수 있습니다.

시뮬레이션에서는 다음 교육용 상수를 사용합니다.

```text
horizontal_velocity(T+36) = 1080
horizontal_bias(T+36)     = 32400  → 변환 성공

horizontal_velocity(T+37) = 1110
horizontal_bias(T+37)     = 33300  → Operand Error
```

이 수치와 단위는 실제 아리안 5의 비행 데이터를 뜻하지 않습니다. 오류 발생 시점과 약 3.5km였던 당시 고도를 알아보기 쉽게 보여주기 위한 학습용 값입니다.

## 프로젝트 구조

```text
c-rocket/
├─ src/main.c       # C 시뮬레이션, 콘솔 로그, CSV 출력, 경계값 테스트
├─ web/             # HTML/CSS/JavaScript/SVG 시각화
├─ output/          # 실행으로 생성되는 CSV
├─ build/           # 실행으로 생성되는 프로그램
└─ run.ps1          # 컴파일, 검증, 웹 서버 실행
```

## 아리안 5 Flight 501과의 관계

1996년 6월 4일 아리안 5의 첫 시험 비행은 약 37초 뒤 유도 및 자세 정보를 잃었습니다. 아리안 4에서 재사용한 관성 기준 장치의 정렬 기능이 아리안 5에서는 이륙 후 필요하지 않았지만 계속 실행되었고, 수평 방향과 관련된 내부값을 64비트 부동소수점에서 16비트 부호 있는 정수로 변환하는 과정에서 표현 범위를 넘었습니다.

백업 SRI 1은 활성 SRI 2보다 한 데이터 주기인 72ms 먼저 같은 원인으로 정지했습니다. 따라서 활성 장치가 정지했을 때 백업 전환이 불가능했고, SRI 2의 진단 비트 패턴 일부가 비행 데이터로 해석되어 노즐이 끝까지 편향되었습니다. 약 T+39에는 20도를 넘은 받음각으로 기체가 파괴되고 자동폭파 장치가 작동했습니다.

실제 관성 기준 장치의 소프트웨어는 C가 아니라 Ada로 작성되었습니다. 이 프로젝트는 해당 사고의 복제본이 아니라, C도 임베디드 시스템에서 널리 사용된다는 점에 착안해 비슷한 형변환 실패와 안전 처리의 차이를 C로 표현한 것입니다.

참고 자료:

- [How Not To Code — A space error: $370 million for an integer overflow](https://hownot2code.wordpress.com/2016/09/02/a-space-error-370-million-for-an-integer-overflow/)
- [YouTube — Ariane 5 Flight 501, 4 June 1996](https://www.youtube.com/watch?v=N6PWATvLQCY)
- [ESA — Flight 501 failure: first information](https://sci.esa.int/web/cluster/-/36899-pr-20-1996-flight-501-failure-first-information)
- [ESA — Learning from Flight 501 and Preparing for 502](https://www.esa.int/esapub/bulletin/bullet89/dalma89.htm)
- [ESA — The Inquiry Board's Recommendations](https://www.esa.int/esapub/bulletin/bullet89/recom89.htm)
- [Ariane 501 Inquiry Board Report 사본](https://www.mssl.ucl.ac.uk/www_plasma/missions/cluster/about_cluster/cluster1/ariane5rep.php)

## 결과 해석

- T+36까지 두 모드의 궤적과 자세는 같습니다.
- UNSAFE에서는 T+36.928에 백업 SRI 1, T+37.000에 활성 SRI 2가 정지합니다.
- T+37에 OBC가 진단 데이터를 받아 노즐 완전 편향을 명령하고 `CONTROL_LOST`가 됩니다.
- T+39에 받음각이 20도를 넘어 기체가 파괴되며, 실제 사고처럼 지상 추락은 따로 계산하지 않습니다.
- SAFE는 정렬 변환을 실행하지 않으며 두 SRI와 정상 비행 데이터를 T+60까지 유지합니다.
