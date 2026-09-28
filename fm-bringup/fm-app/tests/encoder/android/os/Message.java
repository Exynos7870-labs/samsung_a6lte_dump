// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.os;
public class Message {public int what; public Object obj; Handler target;public void sendToTarget(){target.send(this);}}
