# tt-mlir 저장소 분석 리포트

## 프로젝트 개요
- **프로젝트명**: tt-mlir
- **목적**: Tenstorrent AI 가속기를 위한 MLIR 다이얼렉트 정의
- **기반**: MLIR (Multi-Level Intermediate Representation)
- **타겟**: TTNN (tt-metal)

## 프로젝트 목표
- **Generality**: 다양한 AI 모델 및 워크로드 지원 (훈련 포함)
- **Scalable**: 멀티칩 시스템 확장을 위한 퍼스트 클래스 프리미티브
- **Performant**: 뛰어난 기본 성능 제공
- **Tooling**: 인간-컴파일러 협업 최적화 지원
- **Open source**: 모든 개발 과정 공개

## 주요 디렉토리 구조
- `lib/` - 핵심 C++ 라이브러리 및 MLIR 다이얼렉트
- `python/` - Python 바인딩 및 모듈
- `tools/` - 커맨드라인 도구 (ttmlir-opt, ttmlir-translate 등)
- `test/` - 테스트 스위트
- `docs/` - 프로젝트 문서

## 빌드 시스템
- CMake 기반 (최소 3.20.0)
- Clang/Clang++ 권장
- 환경 활성화 필요: `source env/activate`

## 참고 자료
- [공식 문서](https://tenstorrent.github.io/tt-mlir/)
- [Getting Started](https://docs.tenstorrent.com/tt-mlir/getting-started.html)

---
*분석일: $(date)*

## 빌드 결과
### 빌드 성공 (Wed May 14 07:56:33 UTC 2025)
- **ttmlir-opt**: MLIR 최적화 도구
- **ttmlir-translate**: MLIR 번역 도구
- **ttmlir-lsp-server**: LSP 서버
- **Python 바인딩**: 성공적으로 빌드됨

### 빌드 검증
- **ttmlir-opt 실행 성공**: 모든 TT 다이얼렉트 로드됨
- **사용 가능한 TT 다이얼렉트**: tt, ttir, ttkernel, ttmetal, ttnn
- **표준 MLIR 다이얼렉트**: affine, arith, func, linalg, llvm 등 포함

## 결론
tt-mlir 프로젝트 분석 및 빌드가 성공적으로 완료되었습니다. 모든 핵심 기능이 정상 작동합니다.
