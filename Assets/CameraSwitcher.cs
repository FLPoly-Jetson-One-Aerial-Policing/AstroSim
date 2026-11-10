using UnityEngine;

/// <summary>
/// Switches between two cameras with a key press:
///   1) Cockpit camera - a child of the helicopter, sitting on top of the box
///   2) Follow camera  - NOT a child; this script moves it to trail the helicopter
///
/// Put this on an empty GameObject (e.g. "CameraManager").
/// </summary>
public class CameraSwitcher : MonoBehaviour
{
    [Header("Cameras")]
    public Camera cockpitCamera;           // child of Helicopter, on top of the box
    public Camera followCamera;            // separate camera in the scene root
    public Transform target;               // the Helicopter

    [Header("Switching")]
    public KeyCode switchKey = KeyCode.C;
    public bool startWithFollowCamera = true;

    [Header("Follow Camera Settings")]
    public Vector3 offset = new Vector3(0f, 3f, -9f);  // behind and above (in helicopter's heading space)
    public float positionSmooth = 5f;
    public float rotationSmooth = 5f;
    public float lookHeight = 0.5f;                    // aim slightly above the helicopter's center

    bool usingFollow;

    void Start()
    {
        usingFollow = startWithFollowCamera;

        // Snap the follow camera into place so it doesn't fly in from far away
        if (followCamera != null && target != null)
        {
            followCamera.transform.position = GetDesiredPosition();
            followCamera.transform.LookAt(target.position + Vector3.up * lookHeight);
        }

        ApplyCameraState();
    }

    void Update()
    {
        if (Input.GetKeyDown(switchKey))
        {
            usingFollow = !usingFollow;
            ApplyCameraState();
        }
    }

    void LateUpdate()
    {
        // LateUpdate = after everything has moved this frame, so no jitter
        if (!usingFollow || followCamera == null || target == null) return;

        Vector3 desiredPos = GetDesiredPosition();
        followCamera.transform.position = Vector3.Lerp(
            followCamera.transform.position, desiredPos, positionSmooth * Time.deltaTime);

        Vector3 lookPoint = target.position + Vector3.up * lookHeight;
        Quaternion desiredRot = Quaternion.LookRotation(lookPoint - followCamera.transform.position, Vector3.up);
        followCamera.transform.rotation = Quaternion.Slerp(
            followCamera.transform.rotation, desiredRot, rotationSmooth * Time.deltaTime);
    }

    // Use only the helicopter's heading (yaw), so the camera doesn't roll or pitch with it
    Vector3 GetDesiredPosition()
    {
        Quaternion heading = Quaternion.Euler(0f, target.eulerAngles.y, 0f);
        return target.position + heading * offset;
    }

    // Turn one camera on and the other off.
    // Also swap AudioListeners (Unity warns if two are active at once).
    void ApplyCameraState()
    {
        SetCameraActive(cockpitCamera, !usingFollow);
        SetCameraActive(followCamera,   usingFollow);
    }

    void SetCameraActive(Camera cam, bool active)
    {
        if (cam == null) return;
        cam.enabled = active;

        AudioListener listener = cam.GetComponent<AudioListener>();
        if (listener != null) listener.enabled = active;
    }
}
