package org.ngbp.libatsc3.middleware.android.phy;

import org.ngbp.libatsc3.middleware.android.phy.Atsc3NdkPHYClientBase;

public abstract class Atsc3NdkPHYLowaSISStaticJniLoader extends Atsc3NdkPHYClientBase {

    static {
        System.loadLibrary("atsc3_phy_lowasis");
    }
}
