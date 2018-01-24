import avocado, os, signal, subprocess, time
from avocado import Test
from avocado.utils import process

def args_to_shell(args):
    s = ""
    for arg in args:
        if s != "":
            s = s + " "
        s = s + '"' + avocado.utils.astring.shell_escape(arg) + '"'
    return s

@avocado.fail_on(process.CmdError)
def flatpak(*args):
    cmd = args_to_shell(["flatpak"] + list(args))
    return process.run(cmd)

@avocado.fail_on(process.CmdError)
def flatpak_silent(*args):
    cmd = args_to_shell(["flatpak"] + list(args))
    return process.run(cmd, verbose=False)

@avocado.fail_on(process.CmdError)
def flatpak_async(*args):
    cmd = args_to_shell(["flatpak"] + list(args))
    proc = process.SubProcess(cmd=cmd)
    return proc

def needs(what):
    def _needs(f):
        def wrapper(self, *args):
            getattr(self, "ensure_" + what)()
            return f(self, *args)
        return wrapper
    return _needs

class FlatpakTest(Test):

    dbus_address = None
    dbus_pid = -1
    x11_process = None
    needs_system_helper = False
    system_helper = None

    def setUp(self):
        home = self.workdir + "/home"
        os.mkdir(home)
        system = self.workdir + "/system"
        os.mkdir(system)
        user = self.workdir + "/user"
        os.mkdir(user)
        systemcache = self.workdir + "/system-cache"
        os.mkdir(systemcache)

        # We set these to disable e.g. the system dir when doing user stuff
        system_extra = ""
        user_extra = ""

        installation = self.params.get('installation', default='system')
        if installation == 'user':
            self.installation_opt = '--user'
            system_extra = "-disabled"
        elif installation == 'system':
            self.installation_opt = '--system'
            self.needs_system_helper = True
            user_extra = "-disabled"
        else:
            self.error("Unknown installation param %s" % (installation))

        os.environ['FLATPAK_SYSTEM_DIR'] = system + system_extra
        os.environ['FLATPAK_SYSTEM_CACHE_DIR'] = systemcache
        os.environ['FLATPAK_SYSTEM_HELPER_ON_SESSION']="1"
        os.environ['FLATPAK_USER_DIR'] = user + user_extra
        os.environ['HOME'] = home
        os.environ['XDG_CACHE_HOME'] = home + "/cache"
        os.environ['XDG_CONFIG_HOME'] = home + "/config"
        os.environ['XDG_DATA_HOME'] = home + "/share"

        os.unsetenv('DISPLAY')
        os.unsetenv('DBUS_SESSION_BUS_ADDRESS')
        os.unsetenv('DBUS_SESSION_BUS_PID')

        self.apps = zip(*[iter(self.params.get('apps', default=["org.gnome.gedit", "Gedit"]))]*2)


    def ensure_dbus(self):
        if self.dbus_pid != -1:
            return
        res = process.run("dbus-daemon --fork --session --print-address=1 --print-pid=1")
        stdout = res.stdout.split()
        self.dbus_address=stdout[0]
        self.dbus_pid=int(stdout[1])

        os.environ['DBUS_SESSION_BUS_ADDRESS'] = self.dbus_address
        os.environ['DBUS_SESSION_BUS_PID'] = str(self.dbus_pid)

        if self.needs_system_helper:
            self.system_helper = process.SubProcess(cmd="/usr/libexec/flatpak-system-helper --session --no-idle-exit")
            self.system_helper.start()

    def ensure_flathub(self):
        flatpak("remote-add", self.installation_opt, "--if-not-exists", "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo")
    def ensure_x11(self):
        if self.x11_process:
            return

        self.x11_process = subprocess.Popen(["Xvfb", ":42"])
        os.environ['DISPLAY'] = ":42"

    def test_version(self):
        self.fail
        res = flatpak("--version")
        self.assertIn("Flatpak ", res.stdout)
        self.assertIn(".", res.stdout)

    def install(self, name):
        try:
            res = flatpak_silent("info", self.installation_opt, name)
            return
        except:
            pass
        flatpak("install", self.installation_opt, "-y", "flathub", name)
        try:
            res = flatpak_silent("info", self.installation_opt, name)
        except:
            self.fail("Install of %s succeeded, but its not found" % name)

        res = flatpak_silent("list", self.installation_opt)
        if not name in res.stdout:
            self.fail("Installed but not in list")

    @needs("dbus")
    @needs("flathub")
    @needs("x11")
    def test_apps(self):
        for (app, windowname) in self.apps:
            res = self.install(app)
            p = flatpak_async("run", app)
            p.start()

            found_window = False
            stop_time = time.time() + 10
            while time.time() < stop_time:
                p.poll()
                if p.result.exit_status is not None:
                    self.fail("Application %s exited prematurely" % (app))

                r = None
                try:
                    r = process.run("xwininfo -root -children", verbose=False, env={"DISPLAY": ":42"})
                except:
                    pass
                if r and '"%s"' % windowname in r.stdout:
                    found_window = True
                    break

            p.kill()
            if not found_window:
                self.fail("Application %s did not start in time" % (app))

            flatpak("uninstall", self.installation_opt, app)

            res = None
            try:
                res = flatpak_silent("info", self.installation_opt, app)
            except:
                pass
            if res:
                self.fail("Uninstall of %s succeeded, but still not found" % app)

    def tearDown(self):
        if self.dbus_pid >= 0:
            os.kill(self.dbus_pid, signal.SIGINT)
        if self.x11_process:
            self.x11_process.terminate()
        if self.system_helper:
            self.system_helper.terminate()
