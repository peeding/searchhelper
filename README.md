# SearchHelper 3.1.2

**SearchHelper** is a Windows desktop application for searching through local CSV databases and exploring relationships between matching data.

It provides a dark user interface with table and graph views, making it easier to inspect search results and discover connections.

## Features

- **Local CSV Search** — search through CSV files stored on your computer.
- **Table View** — view matching records in a structured table.
- **Relationship Graph** — explore connections between matching data.
- **Graph Screenshot** — capture the relationship graph and copy the image directly to the Windows clipboard.
- **Automatic Data Folder** — the application automatically creates a `data` folder on first launch if it does not exist.
- **Record Details** — select a result to inspect its matching fields.

## Screenshots

_Add application screenshots here._

## Requirements

- Windows 10 or Windows 11
- Microsoft Visual C++ (MSVC)
- CMake
- Visual Studio C++ build tools

## Build from Source

Open **x64 Native Tools Command Prompt for Visual Studio** and run:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The compiled application will be placed in the build output directory.

## Usage

1. Launch SearchHelper.
2. Place your CSV database files inside the `data` folder next to the application executable.
3. Enter your search query.
4. Click **Find**.
5. Switch between Table View and Relationship Graph to explore the results.
6. Use the camera button to copy a screenshot of the graph to the clipboard.

## Project Structure

```text
SearchHelper/
├── data/
├── src/
│   └── main.cpp
├── CMakeLists.txt
└── README.md
```

The `data` folder is created automatically if it is missing.

## Version

**Current version:** 3.1.2

### What's New in 3.1.2

- Added automatic creation of the `data` directory.
- Added graph screenshot capture.
- Added copying graph screenshots directly to the Windows clipboard.

## License

No license has been specified yet.
