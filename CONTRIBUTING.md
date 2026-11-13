# Contributing to CoolTaskManager

First of all, thank you for your interest in contributing. We gladly welcome pull requests from everyone. 

To ensure that your time and effort are well-spent and that your changes align with the project, please follow the guidelines below.

---

## 1. Open an Issue First (Important!)
Before you start writing any code, **please create an issue first**. 
* We currently do not use strict issue templates, so a simple, clear description of the bug or new feature is enough.
* **Why?** This allows us to discuss your idea beforehand and prevents you from spending time on work that might not be needed or could conflict with ongoing changes.

## 2. Setting Up the Development Environment
This is a standard C++ project built with **CMake**. 

1. Clone the repository:
   ```bash
   git clone [https://github.com/Acetylsalicylate/CoolTaskManager.git](https://github.com/Acetylsalicylate/CoolTaskManager.git)
   cd CoolTaskManager
    ```

2. **Dependencies:** All required dependencies are either already included in the project folder or automatically managed via CMake.
3. **Build:** Use the standard C++ build workflow (CMake):
    ```bash
    mkdir build && cd build
    cmake ..
    cmake --build .
    
    ```



## 3. Branching Strategy

Always base your work on a fresh branch originating from `main`.

* **Branch Naming:** Use a prefix corresponding to your task type (e.g., `feature/`, `bugfix/`) followed by the issue number.
* *Examples:* `feature/42`, `bugfix/15`



## 4. Submitting a Pull Request

When your implementation is ready, submit a Pull Request to the `main` branch.

* **Description:** Clearly describe what has changed and why. Reference the related issue (e.g., `Closes #42`).
* **Screenshots:** If your changes introduce visual modifications or UI updates, please attach screenshots or GIFs.
* **Automated Checks:** Please note that we do not have automated CI/CD checks set up yet, so all code reviews are done manually. We appreciate your patience!

Thank you for helping improve the project!
