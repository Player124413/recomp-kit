package dev.recompkit;

import org.libsdl.app.SDLActivity;

/** Loads the host library, which links SDL statically. */
public class RecompActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
