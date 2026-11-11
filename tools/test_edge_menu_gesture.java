// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;


final class TestEdgeMenuGesture {
    private static int checks;
    private static void check(boolean ok, String reason) {
        ++checks; if (!ok) throw new AssertionError(reason);
    }
    public static void main(String[] args) {
        EdgeMenuGesture g = new EdgeMenuGesture(48, 8);
        check(!g.begin(2, 2, 100, 0, false), "Gameplay buttons and points outside the handle retain their gesture");
        check(!g.move(2, 100, 100, 100, 1), "An unclaimed gameplay swipe cannot open the menu");
        check(g.begin(2, 2, 100, 0, true), "Free edge starts a menu candidate");
        check(!g.move(2, 20, 102, 50, 1), "Small motion does not open the menu");
        check(g.move(2, 60, 105, 100, 1), "Rightward swipe opens the menu");
        check(!g.move(2, 80, 105, 120, 1), "One swipe opens only once");
        g.begin(2, 2, 100, 0, true);
        check(!g.move(2, 3, 120, 50, 1), "Vertical gesture cancels the candidate");
        check(!g.move(2, 90, 120, 100, 1), "A cancelled vertical drag cannot become an opening swipe");
        g.begin(2, 20, 100, 0, true);
        check(!g.move(2, 0, 100, 50, 1), "Wrong direction cancels the candidate");
        check(!g.move(2, 90, 100, 100, 1), "Reversing after cancellation cannot open the menu");
        g.begin(7, 2, 100, 0, true);
        check(!g.move(7, 80, 100, 100, 2), "Two fingers cancel the menu gesture");
        check(!g.move(7, 100, 100, 150, 1), "Lifting the second finger does not revive the swipe");
        g.begin(7, 2, 100, 0, true);
        check(!g.move(8, 80, 100, 100, 1), "Another pointer cannot inherit the gesture");
        g.begin(7, 2, 100, 0, true); g.cancel();
        check(!g.move(7, 80, 100, 100, 1), "Focus loss/cancellation prevents a delayed open");
        g.begin(7, 2, 100, 0, true);
        check(!g.move(7, 80, 100, 1001, 1), "An expired edge hold cannot open the menu");
        g.begin(7, 2, 100, 100, true);
        check(!g.move(7, 80, 100, 99, 1), "Invalid event ordering is cancelled");
        g.begin(7, 2, 100, 0, true);
        check(!g.move(7, 80, 140, 100, 1), "Large diagonal is not mistaken for opening");
        check(!g.begin(7, Float.NaN, 100, 0, true), "Nonfinite start is rejected");
        g.begin(7, 2, 100, 0, true);
        check(!g.move(7, Float.POSITIVE_INFINITY, 100, 100, 1), "Nonfinite movement is cancelled");
        g.begin(7, 2, 100, 0, true);
        check(g.move(7, 50, 100, 40, 1), "A fast swipe whose endpoint is ACTION_UP is recognized");
        EdgeMenuGesture dense = new EdgeMenuGesture(144, 24);
        dense.begin(9, 6, 300, 0, true);
        check(!dense.move(9, 120, 300, 100, 1), "Distance scales with display density");
        check(dense.move(9, 150, 310, 120, 1), "Equivalent gesture at high density opens");
        System.out.println("Edge menu passed: " + checks + " gesture and cancellation checks");
    }
}
