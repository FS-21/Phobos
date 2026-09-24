# Phobos C++ Code Styleguide

Official guidelines and conventions from `docs/Project-guidelines-and-policies.md` and Phobos development standards.

---

## 1. Indentation & Spacing
- **Tabs**: Always use tabs (`\t`) for indentation, never spaces.
- **Empty Lines**: Use empty lines to separate code into logical sections:
  - Before `return` statements (except when the function/block is trivial/1-line).
  - Between local variable declarations and the code that uses them.
  - Between consecutive code blocks (braced or braceless).
  - Between hook register acquisition (`GET`, `REF_STACK`) and hook logic.

---

## 2. Bracing & Block Structure (Allman Style)
- **Allman Braces**: Opening curly braces `{` MUST be on their own new line:
  ```cpp
  if (condition)
  {
      DoSomething();
  }
  ```
  Applies to functions, methods, namespaces, classes, structs, loops (`for`, `while`, `do`), conditionals (`if`, `else if`, `else`), and hooks (`DEFINE_HOOK`).
- **Braceless Blocks**: Allowed ONLY when both the condition head AND the body statement are single-line:
  ```cpp
  // OK: Single line condition and body
  if (condition)
      DoSomething();

  // NOT OK: Multiline condition must have braces
  if (longConditionA
      || longConditionB)
  {
      DoSomething();
  }
  ```
- **If-Else Consistency**: If an `if-else` chain is used, all branches must be consistent: either all braced or all braceless.
- **Empty Braces**: Empty brace blocks may be on a single line with an inner space: `{ }`.

---

## 3. Pointers, References & Types
- **Pointer/Reference Attachment**: The asterisk `*` or ampersand `&` MUST be attached to the type, not the variable name:
  ```cpp
  HouseClass* pHouse;        // Correct
  HouseClass *pHouse;        // WRONG
  const auto& item = ref;    // Correct
  ```
- **Type Deduction (`auto`)**:
  - Allowed for complex, verbose, or iterator types if it doesn't hurt readability.
  - NEVER use `auto` for primitive types (`int`, `bool`, `double`, `float`, `char`, etc.).

---

## 4. Naming Conventions
| Element | Convention | Example |
| :--- | :--- | :--- |
| **Classes & Structs** | `PascalCase` | `HouseClass`, `BulletTypeExt` |
| **Entity Classes** | `...Class` postfix | `RadTypeClass`, `SuperWeaponTypeClass` |
| **Extension Classes** | `...Ext` postfix | `HouseExt`, `BuildingTypeExt`, `RulesExt` |
| **Namespaces & Enums** | `PascalCase` | `SessionClass`, `Mission` |
| **Class Fields & Methods** | `PascalCase` | `IsAdvancedAIActive`, `RepairBaseNodes` |
| **Local Variables & Args** | `camelCase` | `targetCell`, `damageMultiplier` |
| **Pointers** | Prefix `p` per level | `pHouse`, `pBuildingType`, `ppCell` |
| **INI Tag Fields in C++** | Dots replaced by `_` | `AdvancedAI.NavalMode` -> `AdvancedAI_NavalMode` |

---

## 5. Member Initializer Lists
Place commas at the beginning of each line after newline to minimize Git merge conflicts:
```cpp
HouseExt(HouseClass* OwnerObject) : AbstractExt(OwnerObject)
	, TargetAlliedFallbackHouse { nullptr }
	, RepairBaseNodes { }
{ }
```

---

## 6. Syringe Hooks Convention
- **Hook Name**: `HookedFunction_HookPurpose` or `ClassName_HookedMethod_HookPurpose`.
- **Return Addresses**: Define as an anonymous enum at the very start of the hook function:
  ```cpp
  DEFINE_HOOK(0x4FE3E9, HouseClass_AI_Building_Intercept, 0x7)
  {
      enum { ReturnCustom = 0x4FE3F0, ReturnDefault = 0 };

      GET(HouseClass*, pHouse, EBP);

      if (HouseExt::IsAdvancedAIActive(pHouse))
      {
          HouseExt::Vinifera_HouseClass_AI_Building(pHouse);
          return ReturnCustom;
      }

      return ReturnDefault;
  }
  ```
- **Hook Size**: Always provide the exact overwritten instruction byte count (last parameter of `DEFINE_HOOK`).

---

## 7. INI Parsing & Serialization Conventions
- **Clean Tag Loading**: Use `Tag.Read(exINI, Section, "TagName")`.
- **No Legacy Hacks**: Do not add ad-hoc `if (exINI.ReadString(...) == 0)` fallbacks for non-standard old names unless explicitly part of an established migration policy.
- **Serialization**: Every persistent tag field in `ExtData` (including `RulesExt`) must be serialized in `Serialize(T& Stm)` via `.Process(this->Tag)`.
