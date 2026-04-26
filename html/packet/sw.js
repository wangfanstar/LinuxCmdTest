const CACHE_NAME='packetgen-offline-v1';
const ASSETS=[
  '../',
  '../PacketGen.html',
  './manifest.webmanifest',
  './CPU2CPP_HDR.json',
  './CPP2CPU_HDR.json',
  './LOOPBACK.json'
];

self.addEventListener('install',(event)=>{
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache)=>cache.addAll(ASSETS)).then(()=>self.skipWaiting())
  );
});

self.addEventListener('activate',(event)=>{
  event.waitUntil(
    caches.keys().then((keys)=>Promise.all(
      keys.filter((k)=>k!==CACHE_NAME).map((k)=>caches.delete(k))
    )).then(()=>self.clients.claim())
  );
});

self.addEventListener('fetch',(event)=>{
  const req=event.request;
  if(req.method!=='GET')return;
  event.respondWith(
    caches.match(req).then((hit)=>{
      if(hit)return hit;
      return fetch(req).then((resp)=>{
        if(!resp||resp.status!==200||resp.type==='opaque')return resp;
        const copy=resp.clone();
        caches.open(CACHE_NAME).then((cache)=>cache.put(req,copy)).catch(()=>{});
        return resp;
      }).catch(()=>caches.match('../PacketGen.html'));
    })
  );
});
