if {[catch {package present Tcl 8.6}]} {return}
package ifneeded Tk 8.6.4 [list load [file join $dir .. libsdl2tk8.6[info sharedlibextension]] Tk]
