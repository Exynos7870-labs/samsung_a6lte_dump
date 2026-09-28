// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.os;
public class Handler {
 public interface Callback {boolean handleMessage(Message m);}
 private final Looper looper;private final Callback cb;
 public Handler(Looper l){this(l,null);} public Handler(Looper l,Callback c){looper=l;cb=c;}
 public Message obtainMessage(int what){return obtainMessage(what,null);}
 public Message obtainMessage(int what,Object obj){Message m=new Message();m.what=what;m.obj=obj;m.target=this;return m;}
 public boolean post(Runnable r){if(looper.quitting)return false;looper.queue.add(r);return true;}
 void send(Message m){post(()->cb.handleMessage(m));}
}
