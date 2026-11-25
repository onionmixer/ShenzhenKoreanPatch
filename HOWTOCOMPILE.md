# 빌드 방법

## 준비물

- Windows 10 이상에서 동작하는 Windows PowerShell 5.1과 Python 3의 `py` 런처
- Visual Studio 2022 또는 Build Tools의 **C++ 데스크톱 개발**, MSVC x86/x64와 Windows SDK
- Visual Studio의 Roslyn C# 컴파일러와 빌드 시 사용할 `Mono.Cecil.dll`
- 지원하는 Windows Steam판 SHENZHEN I/O 원본 설치본
- 사용자 제공 한글 PDF 매뉴얼

스크립트는 설치된 Android .NET SDK 또는 Visual Studio 테스트 플랫폼 폴더에서 Cecil을 찾습니다. 별도 설치본을 사용하려면 `-CecilPath`에 DLL의 경로를 지정합니다. 로컬 빌드에서 확인한 Cecil은 `.NET Android SDK 35.0.78`에 포함된 버전입니다.

한글 PDF는 `inputs/SHENZHEN IO Manual (Korean).pdf`에 넣거나 `-ManualPath`로 지정합니다. 파일 이름에 관계없이 PDF의 SHA-256은 `b2dda4ada609f1bc766d2983e3199c9c65167acfcf1c33d6803b4614c29db121`이어야 합니다. [한글 매뉴얼 출처](https://blog.naver.com/dpotudf0/220956199248)

## 컴파일

저장소 루트에서 PowerShell로 실행합니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -GamePath 'D:\SteamLibrary\steamapps\common\SHENZHEN IO'
```

PDF와 Cecil 경로를 직접 지정할 수 있습니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -GamePath 'D:\SteamLibrary\steamapps\common\SHENZHEN IO' `
  -ManualPath 'C:\빌드입력\SHENZHEN IO 매뉴얼.pdf' `
  -CecilPath 'C:\빌드도구\Mono.Cecil.dll'
```

스크립트가 게임의 원본 해시를 확인하고, `build/`에서 실행 파일 변경분과 내장 리소스를 생성합니다. Win32 패처는 MSVC `/MT`로 C/C++ 런타임을 정적으로 링크하고, x86 Windows GUI 실행 파일로 빌드합니다. 빌드 도구는 컴파일할 때만 필요합니다. 게임의 관리 코드 보조 DLL은 패처에 내장되고 게임이 사용하는 기존 CLR에서 로드됩니다.

## 결과물

- `release/ShenzhenKoreanPatcher.exe` · 단일 실행 파일
- `release/ShenzhenKoreanPatch-v0.8-win32.zip` · 같은 실행 파일을 압축한 배포본

버전은 루트의 `VERSION`에서 읽습니다. 빌드 중간 산출물, 원본 PDF 입력, 배포 바이너리는 Git에서 제외됩니다.

## 디렉터리

| 경로 | 내용 |
| --- | --- |
| `src/native/` | Win32 GUI, SHA-256, 패치 적용·복원 및 바로가기 처리 |
| `src/managed/` | Cecil 변환기와 게임에서 읽는 관리 코드 보조 DLL의 소스 |
| `localization/` | 한국어 텍스트와 이미지, 지원 원본 파일 해시 |
| `tools/` | 실행 파일 변경분, 내장 리소스와 ZIP을 만드는 스크립트 |
| `docs/` | README 스크린샷 |
