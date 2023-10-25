/*
	Quattrocento LSL App streams the data from an OTB Quattrocento amplifier to the network via LSL

    Copyright (C) 2023 patrick@ofner.science

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/


// entry point to the application, it parses the command line arguments and lets Qt handle the rest
#ifdef _WIN32
#include <Windows.h>
#endif
#include "mainwindow.hpp"
#include <QApplication>

int main(int argc, char *argv[]) {
	// print output to console window if it exits
	#ifdef _WIN32
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
	}
	#endif

	QApplication app(argc, argv);
	MainWindow win(nullptr, argc > 1 ? argv[1] : nullptr);
	win.show();
	return app.exec();
}
